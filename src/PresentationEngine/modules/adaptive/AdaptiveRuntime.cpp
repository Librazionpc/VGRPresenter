#include "modules/adaptive/AdaptiveRuntime.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/resources/ResourceManager.hpp"
#include "core/threading/ThreadPool.hpp"
#include "modules/content/ContentManager.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace bps::adaptive {

AdaptiveRuntime& AdaptiveRuntime::Instance() {
    static AdaptiveRuntime instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

namespace {
// Refresh the hardware view AND the stable snapshot the optimizer is bound to.
void RefreshHardware(HardwareProfiler& hw, HardwareInfo& snapshot,
                     CapabilityDetector& caps, GPURuntime& gpu) {
    hw.Refresh();
    snapshot = hw.Info();      // stable copy under the profiler's lock
    caps.SetHardware(snapshot);
    gpu.SetGpuAvailable(snapshot.gpuDetected);
}
} // namespace

Result<void> AdaptiveRuntime::Initialize() {
    if (initialized_.load()) return Ok();

    // Gather the hardware view from the PAL and derive capabilities.
    RefreshHardware(hardware_, hwSnapshot_, caps_, gpu_);

    auto& logger = Logger::Instance();
    logger.Info(std::format("AdaptiveRuntime: {} cores, {} MB RAM{}", hardware_.CoreCount(),
                            hardware_.TotalRamBytes() / (1024 * 1024),
                            hardware_.Info().gpuDetected
                                ? ", GPU: " + hardware_.Info().gpuName
                                : ", no GPU backend"),
                "Adaptive");

    // Feature registry: built-in feature metadata (docs/specs/16 §8).
    const auto& hw = hwSnapshot_;
    const auto mb = 1024ull * 1024ull;
    const auto gb = 1024ull * 1024ull * 1024ull;
    ModuleContract aiContract;
    aiContract.name = "ai";
    aiContract.minRamBytes = 512 * mb;
    aiContract.recommendedRamBytes = 2 * gb;
    aiContract.maxRamBytes = 4 * gb;
    aiContract.gpuOptional = true;
    aiContract.cpuThreads = 2;
    aiContract.estimatedStartupMs = 400;
    aiContract.supportsLazyLoading = true;
    aiContract.supportsSuspension = true;
    (void)features_.Register({"ai", "AI Search", aiContract, FeatureState::Enabled, true});

    ModuleContract streamingContract;
    streamingContract.name = "streaming";
    streamingContract.minRamBytes = 128 * mb;
    streamingContract.recommendedRamBytes = 512 * mb;
    streamingContract.maxRamBytes = gb;
    streamingContract.gpuOptional = true;
    streamingContract.cpuThreads = 2;
    streamingContract.estimatedStartupMs = 250;
    streamingContract.supportsLazyLoading = true;
    streamingContract.supportsSuspension = true;
    (void)features_.Register({"streaming", "Streaming", streamingContract,
                              FeatureState::Enabled, true});

    ModuleContract remoteContract;
    remoteContract.name = "remote";
    remoteContract.minRamBytes = 32 * mb;
    remoteContract.recommendedRamBytes = 128 * mb;
    remoteContract.maxRamBytes = 256 * mb;
    remoteContract.gpuOptional = true;
    remoteContract.cpuThreads = 1;
    remoteContract.estimatedStartupMs = 150;
    remoteContract.supportsLazyLoading = true;
    remoteContract.supportsSuspension = true;
    (void)features_.Register({"remote", "Remote Control", remoteContract,
                              FeatureState::Enabled, true});

    ModuleContract ndiContract;
    ndiContract.name = "ndi";
    ndiContract.minRamBytes = 128 * mb;
    ndiContract.recommendedRamBytes = 512 * mb;
    ndiContract.maxRamBytes = gb;
    ndiContract.gpuOptional = true;
    ndiContract.cpuThreads = 2;
    ndiContract.estimatedStartupMs = 300;
    ndiContract.supportsLazyLoading = true;
    ndiContract.supportsSuspension = true;
    (void)features_.Register({"ndi", "NDI Output", ndiContract, FeatureState::Enabled, true});

    ModuleContract cloudContract;
    cloudContract.name = "cloud";
    cloudContract.minRamBytes = 64 * mb;
    cloudContract.recommendedRamBytes = 256 * mb;
    cloudContract.maxRamBytes = 512 * mb;
    cloudContract.gpuOptional = true;
    cloudContract.cpuThreads = 1;
    cloudContract.estimatedStartupMs = 200;
    cloudContract.supportsLazyLoading = true;
    cloudContract.supportsSuspension = true;
    (void)features_.Register({"cloud", "Cloud Sync", cloudContract, FeatureState::Enabled, true});
    // Every built-in feature is auto-evaluated against the machine: heavy
    // features (AI, NDI, streaming) are disabled when the host cannot host them.
    for (const auto& def : features_.All()) {
        if (def.enabledByDefault) {
            auto rec = features_.RecommendState(def, hw.totalRamBytes, hw.gpuDetected);
            if (rec == FeatureState::Disabled) (void)features_.SetEnabled(def.id, false);
        }
    }

    // Wiring: learning persistence + smart cache + startup optimizer.
    smartCache_.SetLearning(&learning_);
    startup_.SetLearning(&learning_);
    if (auto r = learning_.Load(); !r.ok())
        logger.Info("AdaptiveRuntime: no previous usage history", "Adaptive");

    // Bind the optimizer to the live inputs (hardware snapshot, capabilities,
    // profiler, budgets, modes, settings + its guard).
    optimizer_.Bind(&hwSnapshot_, &caps_, &profiler_, &budgets_, &modes_, &settings_,
                    &settingsMutex_);

    // Initial compute of budgets + quality from the machine.
    const PressureLevel p = ResourceManager::Instance().MemoryPressure();
    activePressure_.store(p);
    Recompute(hw.totalRamBytes, p);
    activeQuality_.store(optimizer_.GetQualityLevel());

    initialized_.store(true);
    return Ok();
}

Result<void> AdaptiveRuntime::Start() {
    if (running_.load()) return Ok();
    WireEvents();

    // Adaptation heartbeat: Collect → Analyze → Optimize every 3 s.
    auto& sched = TaskScheduler::Instance();
    auto r = sched.ScheduleEvery([this] { Heartbeat(); }, std::chrono::seconds(3));
    if (!r.ok()) return Error::Make(Err::Scheduler_Submit, "Adaptive",
                                    "heartbeat: " + r.error().message);
    heartbeatTask_.store(r.value());

    // Apply the startup configuration (thread count, budgets) once.
    (void)ApplyOptimization("startup");

    running_.store(true);
    Logger::Instance().Info("AdaptiveRuntime started (mode=" +
                                std::string(ToString(modes_.Mode())) + ", layer=" +
                                std::string(ToString(modes_.Layer())) + ")",
                            "Adaptive");
    return Ok();
}

Result<void> AdaptiveRuntime::Stop() {
    if (!running_.load()) return Ok();
    auto id = heartbeatTask_.load();
    if (id != 0) (void)TaskScheduler::Instance().Cancel(id);
    heartbeatTask_.store(0);
    UnwireEvents();
    running_.store(false);
    return Ok();
}

Result<void> AdaptiveRuntime::Shutdown() {
    (void)Stop();
    // Persist learned usage so the next session starts smart.
    (void)learning_.Save();
    return Ok();
}

Result<void> AdaptiveRuntime::Reload() {
    // Re-read configuration: user mode, layer, preference.
    auto& config = ConfigurationManager::Instance();
    const std::string mode = config.GetString("adaptive.mode", "Automatic");
    const std::string layer = config.GetString("adaptive.layer", "Automatic");
    const std::string pref = config.GetString("adaptive.preference", "None");

    static const auto findMode = [](const std::string& s) -> UserMode {
        for (int i = 0; i <= static_cast<int>(UserMode::Custom); ++i) {
            if (ToString(static_cast<UserMode>(i)) == s) return static_cast<UserMode>(i);
        }
        return UserMode::Automatic;
    };
    static const auto findLayer = [](const std::string& s) -> ConfigLayer {
        for (int i = 0; i <= static_cast<int>(ConfigLayer::Expert); ++i) {
            if (ToString(static_cast<ConfigLayer>(i)) == s) return static_cast<ConfigLayer>(i);
        }
        return ConfigLayer::Automatic;
    };
    static const auto findPref = [](const std::string& s) -> Preference {
        for (int i = 0; i <= static_cast<int>(Preference::PreferLowMemory); ++i) {
            if (ToString(static_cast<Preference>(i)) == s) return static_cast<Preference>(i);
        }
        return Preference::None;
    };
    modes_.SetMode(findMode(mode));
    modes_.SetLayer(findLayer(layer));
    modes_.SetPreference(findPref(pref));

    // AI default from config.
    const bool aiEnabled = config.GetBool("adaptive.features.ai", true);
    if (!aiEnabled) (void)features_.SetEnabled("ai", false);

    RefreshHardware(hardware_, hwSnapshot_, caps_, gpu_);

    (void)ApplyOptimization("config-reload");
    Logger::Instance().Info("AdaptiveRuntime reloaded (mode=" + std::string(mode) + ")",
                            "Adaptive");
    return Ok();
}

Result<void> AdaptiveRuntime::Reset() {
    (void)Stop();
    modes_.SetMode(UserMode::Automatic);
    modes_.SetLayer(ConfigLayer::Automatic);
    modes_.SetPreference(Preference::None);
    (void)features_.Unregister("ai");
    (void)features_.Unregister("streaming");
    (void)features_.Unregister("remote");
    (void)features_.Unregister("ndi");
    (void)features_.Unregister("cloud");
    profiler_.Reset();
    running_.store(false);
    initialized_.store(false);   // allow a fresh Initialize() (re-registers features)
    return Ok();
}

HealthReport AdaptiveRuntime::GetHealth() const {
    HealthReport h;
    h.state = running_.load() ? HealthState::Healthy : HealthState::Degraded;
    h.detail = "mode=" + std::string(ToString(modes_.Mode())) + " quality=" +
               std::string(ToString(activeQuality_.load()));
    h.errorCount = errorCount_.load();
    return h;
}

Metrics AdaptiveRuntime::MetricsSnapshot() const {
    Metrics m;
    auto s = profiler_.Snapshot();
    m.cpuPct = s.cpuPct;
    m.ramBytes = s.ramUsedBytes;
    m.threadCount = ThreadPool::Instance().WorkerCount();
    m.queueLength = optimizationCount_.load();
    m.errorCount = errorCount_.load();
    m.health = running_.load() ? HealthState::Healthy : HealthState::Degraded;
    return m;
}

// ---------------------------------------------------------------------------
// Hardware & capabilities
// ---------------------------------------------------------------------------

const HardwareInfo& AdaptiveRuntime::Hardware() const { return hardware_.Info(); }

bool AdaptiveRuntime::Supports(Capability c) const { return caps_.Supports(c); }

std::vector<Capability> AdaptiveRuntime::Capabilities() const { return caps_.Supported(); }

// ---------------------------------------------------------------------------
// The questions
// ---------------------------------------------------------------------------

unsigned AdaptiveRuntime::GetRecommendedThreadCount() const {
    return optimizer_.GetRecommendedThreadCount();
}
uint64_t AdaptiveRuntime::GetTextureBudget() const { return optimizer_.GetTextureBudget(); }
uint64_t AdaptiveRuntime::GetCacheBytes() const { return optimizer_.GetCacheBytes(); }
uint64_t AdaptiveRuntime::GetRenderCacheBytes() const { return optimizer_.GetRenderCacheBytes(); }
size_t AdaptiveRuntime::GetBatchSize() const { return optimizer_.GetBatchSize(); }
bool AdaptiveRuntime::ShouldUseHardwareDecoder() const {
    return optimizer_.ShouldUseHardwareDecoder();
}
bool AdaptiveRuntime::ShouldUseGpu(GpuTask task) const { return optimizer_.ShouldUseGpu(task); }
int AdaptiveRuntime::GetAIModelTier() const { return optimizer_.GetAIModelTier(); }
int AdaptiveRuntime::GetSearchTier() const { return optimizer_.GetSearchTier(); }
int AdaptiveRuntime::GetStreamingQuality() const { return optimizer_.GetStreamingQuality(); }
unsigned AdaptiveRuntime::GetThumbnailResolutionPct() const {
    return optimizer_.GetThumbnailResolutionPct();
}
bool AdaptiveRuntime::ShouldPreloadFrequentlyUsed() const {
    return optimizer_.ShouldPreloadFrequentlyUsed();
}
bool AdaptiveRuntime::BackgroundWorkAllowed() const { return optimizer_.BackgroundWorkAllowed(); }
QualityLevel AdaptiveRuntime::GetQualityLevel() const { return optimizer_.GetQualityLevel(); }

// ---------------------------------------------------------------------------
// User control
// ---------------------------------------------------------------------------

Result<void> AdaptiveRuntime::SetUserMode(UserMode mode) {
    modes_.SetMode(mode);
    return ApplyOptimization("user-mode");
}

UserMode AdaptiveRuntime::GetUserMode() const { return modes_.Mode(); }

Result<void> AdaptiveRuntime::SetConfigLayer(ConfigLayer layer) {
    modes_.SetLayer(layer);
    return Ok();
}

ConfigLayer AdaptiveRuntime::GetConfigLayer() const { return modes_.Layer(); }

Result<void> AdaptiveRuntime::SetPreference(Preference p) {
    modes_.SetPreference(p);
    return ApplyOptimization("user-preference");
}

Preference AdaptiveRuntime::GetPreference() const { return modes_.GetPreference(); }

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------

Result<void> AdaptiveRuntime::RegisterFeature(const FeatureDef& def) {
    return features_.Register(def);
}
Result<void> AdaptiveRuntime::SetFeatureEnabled(std::string_view id, bool enabled) {
    auto r = features_.SetEnabled(id, enabled);
    if (r.ok()) {
        auto& bus = EventBus::Instance();
        if (enabled)
            (void)bus.Publish(events::AdaptiveFeatureEnabled{std::string(id)});
        else
            (void)bus.Publish(events::AdaptiveFeatureDisabled{std::string(id), "user"});
    }
    return r;
}
Result<void> AdaptiveRuntime::SetFeatureSuspended(std::string_view id, bool suspended) {
    auto r = features_.SetSuspended(id, suspended);
    if (r.ok()) {
        auto& bus = EventBus::Instance();
        if (suspended)
            (void)bus.Publish(events::AdaptiveModuleSuspended{std::string(id), "runtime"});
        else
            (void)bus.Publish(events::AdaptiveModuleResumed{std::string(id)});
    }
    return r;
}
Result<FeatureState> AdaptiveRuntime::GetFeatureState(std::string_view id) const {
    return features_.State(id);
}
std::vector<FeatureDef> AdaptiveRuntime::Features() const { return features_.All(); }

// ---------------------------------------------------------------------------
// Learning
// ---------------------------------------------------------------------------

void AdaptiveRuntime::RecordUsage(std::string_view kind, std::string_view id) {
    learning_.RecordUsage(kind, id);
}

std::vector<std::string> AdaptiveRuntime::PreloadCandidates(std::string_view kind,
                                                            int limit) const {
    return smartCache_.PreloadCandidates(kind, limit);
}

bool AdaptiveRuntime::ShouldPreload(std::string_view kind, std::string_view id) const {
    return smartCache_.ShouldPreload(kind, id);
}

Result<void> AdaptiveRuntime::SaveLearning() { return learning_.Save(); }
Result<void> AdaptiveRuntime::LoadLearning() { return learning_.Load(); }

// ---------------------------------------------------------------------------
// Optimization
// ---------------------------------------------------------------------------

Result<void> AdaptiveRuntime::ApplyOptimization(std::string_view reason) {
    // Serialize with the heartbeat and event publishers (budgets_ → ThreadPool
    // → ResourceManager → CAMS mutex ordering stays consistent).
    std::lock_guard<std::mutex> applyLock(applyMutex_);

    const auto& hw = hwSnapshot_;
    const PressureLevel p = ResourceManager::Instance().MemoryPressure();
    activePressure_.store(p);

    // 1. Recompute quality settings + budgets from the current state.
    Recompute(hw.totalRamBytes, p);

    // 2. Apply the thread-count recommendation to the global ThreadPool.
    const unsigned threads = optimizer_.GetRecommendedThreadCount();
    if (threads > 0 && threads != ThreadPool::Instance().WorkerCount()) {
        (void)ThreadPool::Instance().Resize(threads);
    }

    // 3. Apply the cache budget to CAMS (content cache shrink/grow).
    auto& content = content::ContentManager::Instance();
    content.SetCacheCapacity(optimizer_.GetCacheBytes());

    // 4. Map pressure + mode onto the ResourceManager mode.
    ResourceMode rm = ResourceMode::Balanced;
    switch (modes_.Mode()) {
        case UserMode::Performance: rm = ResourceMode::Performance; break;
        case UserMode::Battery:     rm = ResourceMode::Battery; break;
        case UserMode::Developer:   rm = ResourceMode::Developer; break;
        case UserMode::SafeMode:    rm = ResourceMode::Strict; break;
        default:
            rm = (p >= PressureLevel::High) ? ResourceMode::Strict : ResourceMode::Balanced;
            break;
    }
    (void)ResourceManager::Instance().SetMode(rm);

    // 5. Publish on quality change.
    const QualityLevel q = optimizer_.GetQualityLevel();
    const QualityLevel prev = activeQuality_.exchange(q);
    if (q != prev)
        (void)EventBus::Instance().Publish(
            events::AdaptiveQualityChanged{ToString(q), std::string(reason)});

    ++optimizationCount_;
    if (optimizationCount_.load() % 10 == 1)
        (void)EventBus::Instance().Publish(
            events::AdaptiveOptimizationApplied{std::format("quality={} threads={} reason={}",
                                                             ToString(q), threads, reason)});
    return Ok();
}

// ---------------------------------------------------------------------------
// Recompute
// ---------------------------------------------------------------------------

void AdaptiveRuntime::Recompute(uint64_t totalRamBytes, PressureLevel pressure) {
    // Custom mode keeps the previous settings as the base (user overrides are
    // applied on top of a profile per docs/specs/16 §6); every other mode
    // recomputes from its base level. One lock scope: settings_ write + budgets
    // derive from the same consistent snapshot.
    std::lock_guard<std::mutex> lock(settingsMutex_);
    if (optimizer_.GetQualityLevel() != QualityLevel::Custom) {
        settings_ = SettingsFor(optimizer_.GetQualityLevel(), totalRamBytes,
                                hwSnapshot_.coreCount, gpu_.GpuAvailable());
    }
    budgets_.Recompute(totalRamBytes, settings_, pressure);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void AdaptiveRuntime::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ResourcePressureChanged>(
        [this](const events::ResourcePressureChanged& e) { OnPressure(e); }));
    subscriptions_.push_back(bus.Subscribe<events::PowerChanged>(
        [this](const events::PowerChanged& e) { OnPowerChanged(e); }));
    subscriptions_.push_back(bus.Subscribe<events::BatteryLow>(
        [this](const events::BatteryLow& e) { OnBatteryLow(e); }));
    subscriptions_.push_back(bus.Subscribe<events::MonitorConnected>(
        [this](const events::MonitorConnected& e) { OnMonitorConnected(e); }));
    subscriptions_.push_back(bus.Subscribe<events::MonitorDisconnected>(
        [this](const events::MonitorDisconnected& e) { OnMonitorDisconnected(e); }));
    subscriptions_.push_back(bus.Subscribe<events::SystemSleep>(
        [this](const events::SystemSleep& e) { OnSleep(e); }));
    subscriptions_.push_back(bus.Subscribe<events::SystemWake>(
        [this](const events::SystemWake& e) { OnWake(e); }));
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }));
}

void AdaptiveRuntime::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
}

void AdaptiveRuntime::OnPressure(const events::ResourcePressureChanged& e) {
    if (e.resource != "memory") return;
    activePressure_.store(e.to);
    memPressure_.Update(e.to);
    Logger::Instance().Debug("AdaptiveRuntime: memory pressure " +
                                 std::string(ToString(e.to)),
                             "Adaptive");
    (void)ApplyOptimization("memory-pressure");
}

void AdaptiveRuntime::OnPowerChanged(const events::PowerChanged& e) {
    // Prefer the authoritative PAL reading over parsing the human-readable
    // detail string ("on battery (87%)" — % may not be present).
    bool onBattery = e.detail.contains("on battery");
    int pct = -1;
    try {
        auto power = platform::PlatformAccessor::Get().Power().Current();
        onBattery = power.onBattery;
        pct = power.batteryPercent;
    } catch (...) {
    }
    power_.Update(true, onBattery, pct);
    if (onBattery) {
        // Battery saver quality profile (docs/specs/16 §14).
        modes_.SetMode(UserMode::Battery);
        (void)ApplyOptimization("battery");
    }
}

void AdaptiveRuntime::OnBatteryLow(const events::BatteryLow& e) {
    power_.Update(true, true, -1);
    modes_.SetMode(UserMode::Battery);
    Logger::Instance().Warning("AdaptiveRuntime: battery low (" + e.detail + ") — throttling",
                               "Adaptive");
    (void)ApplyOptimization("battery-low");
}

void AdaptiveRuntime::OnMonitorConnected(const events::MonitorConnected& e) {
    RefreshHardware(hardware_, hwSnapshot_, caps_, gpu_);
    Logger::Instance().Debug("AdaptiveRuntime: monitor connected (" + e.monitorId + ")",
                             "Adaptive");
    (void)ApplyOptimization("monitor-connected");
}

void AdaptiveRuntime::OnMonitorDisconnected(const events::MonitorDisconnected& e) {
    RefreshHardware(hardware_, hwSnapshot_, caps_, gpu_);
    Logger::Instance().Debug("AdaptiveRuntime: monitor disconnected (" + e.monitorId + ")",
                             "Adaptive");
    (void)ApplyOptimization("monitor-disconnected");
}

void AdaptiveRuntime::OnSleep(const events::SystemSleep&) {
    // Pause background work while asleep.
    modes_.SetMode(UserMode::Battery);
    (void)ApplyOptimization("sleep");
}

void AdaptiveRuntime::OnWake(const events::SystemWake&) {
    RefreshHardware(hardware_, hwSnapshot_, caps_, gpu_);
    if (modes_.Mode() == UserMode::Battery) modes_.SetMode(UserMode::Automatic);
    (void)ApplyOptimization("wake");
}

void AdaptiveRuntime::OnConfigReload(const events::ConfigHotReload&) {
    (void)Reload();
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void AdaptiveRuntime::Heartbeat() {
    if (!running_.load()) return;

    // Collect: PAL snapshot + subsystem-reported metrics.
    platform::Snapshot sample{};
    if (platform::PlatformAccessor::Installed())
        sample = platform::PlatformAccessor::Get().Sample();
    const auto prof = profiler_.Snapshot();

    // Real CPU% from jiffies deltas (fall back to the recorded value when the
    // PAL backend has no cumulative counters).
    double cpuPct = prof.cpuPct;
    if (sample.cpuTotalJiffies > 0) {
        const uint64_t prevTotal = prevCpuTotal_.exchange(sample.cpuTotalJiffies);
        const uint64_t prevIdle = prevCpuIdle_.exchange(sample.cpuIdleJiffies);
        if (prevTotal > 0 && sample.cpuTotalJiffies > prevTotal) {
            const double dTotal =
                static_cast<double>(sample.cpuTotalJiffies - prevTotal);
            const double dIdle = static_cast<double>(sample.cpuIdleJiffies - prevIdle);
            cpuPct = std::min(100.0, std::max(0.0, 100.0 * (1.0 - dIdle / dTotal)));
        }
    }
    profiler_.RecordCpuPct(cpuPct);
    profiler_.RecordMemory(sample.totalRamBytes - sample.availableRamBytes,
                           sample.totalRamBytes);
    if (sample.batteryPercent >= 0)
        power_.Update(true, sample.onBattery, sample.batteryPercent);

    // Analyze: thermal data (if exposed) + memory pressure.
    if (prof.cpuTemperatureC > 0) thermal_.Update(prof.cpuTemperatureC);
    memPressure_.Update(ResourceManager::Instance().MemoryPressure());

    // Optimize only when something meaningful changed (avoid churn).
    const bool batteryThrottle = power_.ShouldThrottle();
    const bool thermalThrottle = thermal_.ShouldThrottle();
    const bool memHigh = memPressure_.Level() >= PressureLevel::High;
    const bool presenting = modes_.Mode() == UserMode::Presentation;

    if (batteryThrottle || thermalThrottle || memHigh || presenting)
        (void)ApplyOptimization(batteryThrottle ? "battery"
                                  : thermalThrottle ? "thermal"
                                  : memHigh       ? "memory"
                                                  : "presentation");
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

RuntimeSnapshot AdaptiveRuntime::Snapshot() const {
    RuntimeSnapshot s;
    s.mode = ToString(modes_.Mode());
    s.layer = ToString(modes_.Layer());
    s.quality = ToString(activeQuality_.load());
    const auto& hw = hwSnapshot_;
    s.gpuName = hw.gpuName;
    s.gpuAvailable = gpu_.GpuAvailable();
    s.cores = hw.coreCount;
    s.totalRamBytes = hw.totalRamBytes;
    const auto prof = profiler_.Snapshot();
    s.ramUsedBytes = prof.ramUsedBytes;
    s.vramUsedBytes = prof.vramUsedBytes;
    s.vramTotalBytes = prof.vramTotalBytes;
    s.cpuPct = prof.cpuPct;
    s.gpuPct = prof.gpuPct;
    s.frameTimeMs = prof.frameTimeMs;
    s.temperatureC = prof.cpuTemperatureC;
    s.workerThreads = ThreadPool::Instance().WorkerCount();
    s.activeFeatures = features_.Count();
    for (const auto& f : features_.All())
        if (f.state == FeatureState::Suspended) ++s.suspendedFeatures;
    s.memoryBudgetBytes = budgets_.Total();
    s.budgets = budgets_.All();
    s.preloadCandidates = smartCache_.PreloadCandidates("module", 5);
    s.recommendations = recommendations_.Recommend(
        gpu_.GpuAvailable(), activeQuality_.load(), hw.totalRamBytes, power_.ShouldThrottle());
    return s;
}

} // namespace bps::adaptive
