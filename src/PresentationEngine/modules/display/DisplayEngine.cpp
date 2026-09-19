#include "modules/display/DisplayEngine.hpp"

#include "core/logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace bps::display {

DisplayEngine& DisplayEngine::Instance() {
    static DisplayEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> DisplayEngine::Initialize() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_.load()) return Ok();
        initialized_.store(true);
        // Default providers: physical (Linux) + virtual + null (headless) +
        // NDI (Phase 17 broadcast output; software-loopback when no SDK).
        providers_.push_back(std::make_shared<LinuxDisplayProvider>());
        providers_.push_back(std::make_shared<VirtualDisplayProvider>());
        providers_.push_back(std::make_shared<NullDisplayProvider>());
        providers_.push_back(std::make_shared<NdiDisplayProvider>());
        // Initial device snapshot.
        for (const auto& provider : providers_)
            for (const auto& d : provider->Enumerate()) devices_[d.id] = d;
    }
    (void)profiles_.Initialize();
    WireEvents();
    return Ok();
}

Result<void> DisplayEngine::Start() {
    if (running_.load()) return Ok();
    running_.store(true);
    // Hot-plug heartbeat (2s) — the PAL event watcher also publishes
    // platform.monitor_* which OnMonitor*() consumes immediately.
    auto res = TaskScheduler::Instance().ScheduleEvery(
        [this]() { HotplugTick(); }, std::chrono::seconds(2));
    if (!res.ok()) {
        Logger::Instance().Warning("DisplayEngine hotplug tick: " + res.error().message,
                                   "DisplayEngine");
    } else {
        hotplugTask_.store(res.value());
    }
    return Ok();
}

Result<void> DisplayEngine::Stop() {
    running_.store(false);
    if (hotplugTask_.load() != 0) {
        (void)TaskScheduler::Instance().Cancel(hotplugTask_.load());
        hotplugTask_.store(0);
    }
    return Ok();
}

Result<void> DisplayEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    (void)Stop();
    UnwireEvents();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        providers_.clear();
        devices_.clear();
        router_.Clear();
        initialized_.store(false);
    }
    (void)profiles_.Shutdown();
    return Ok();
}

Result<void> DisplayEngine::Reload() {
    (void)RefreshDevices();
    return Ok();
}

Result<void> DisplayEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
    return Ok();
}

HealthReport DisplayEngine::GetHealth() const {
    HealthReport r;
    r.state = HealthState::Healthy;
    r.detail = std::format("providers={} devices={} outputs={}", providers_.size(),
                           devices_.size(), OutputCount());
    return r;
}

Metrics DisplayEngine::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = framesRouted_.load();
    m.errorCount = errorCount_.load();
    m.threadCount = 0;
    m.health = HealthState::Healthy;
    return m;
}

// ---------------------------------------------------------------------------
// Providers
// ---------------------------------------------------------------------------
Result<void> DisplayEngine::RegisterProvider(std::shared_ptr<IDisplayProvider> provider) {
    if (!provider) return Error::Make(Err::InvalidArgument, "DisplayEngine", "null provider");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : providers_)
        if (std::string_view(p->Name()) == provider->Name())
            return Error::Make(Err::Display_ProviderExists, "DisplayEngine",
                               "provider '" + std::string(provider->Name()) + "' already registered");
    providers_.push_back(std::move(provider));
    // Enumerate runs under the lock by design (registration is the one place a
    // provider may touch the engine registry synchronously; providers never call
    // back into the engine, so this cannot deadlock).
    for (const auto& d : providers_.back()->Enumerate()) devices_[d.id] = d;
    return Ok();
}

Result<void> DisplayEngine::UnregisterProvider(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = providers_.begin(); it != providers_.end(); ++it) {
        if (std::string_view((*it)->Name()) == name) {
            providers_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Display_ProviderNotFound, "DisplayEngine",
                       "provider '" + std::string(name) + "' not found");
}

std::vector<std::string> DisplayEngine::ProviderNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& p : providers_) names.push_back(p->Name());
    return names;
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------
std::vector<DisplayDevice> DisplayEngine::Devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DisplayDevice> out;
    out.reserve(devices_.size());
    for (const auto& [_, d] : devices_) out.push_back(d);
    return out;
}

Result<DisplayDevice> DisplayEngine::GetDevice(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(id);
    if (it == devices_.end())
        return Error::Make(Err::Display_DeviceNotFound, "DisplayEngine",
                           "device '" + std::string(id) + "' not found");
    return it->second;
}

const DisplayDevice* DisplayEngine::FindDeviceLocked(std::string_view id) const {
    auto it = devices_.find(id);
    return it == devices_.end() ? nullptr : &it->second;
}

Result<void> DisplayEngine::RefreshDevices() {
    // Snapshot the registry under the lock (keeps the providers alive), then
    // probe outside the lock: providers never call back into the engine, so the
    // lock is only held for the snapshot and the registry diff. This avoids both
    // a deadlock (probe under lock) and a race with Register/UnregisterProvider.
    std::vector<std::shared_ptr<IDisplayProvider>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = providers_;
    }
    std::vector<DisplayDevice> probeResults;
    for (auto& provider : snapshot) {
        auto probe = provider->Probe();
        if (!probe.ok()) {
            errorCount_.fetch_add(1);
            continue;
        }
        for (auto& d : probe.value()) probeResults.push_back(d);
    }
    std::vector<DisplayDevice> changed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& d : probeResults) {
            auto it = devices_.find(d.id);
            bool existed = it != devices_.end();
            if (!existed) {
                // A disconnected device we never knew about has nothing to
                // disconnect; only register connected devices.
                if (!d.connected) continue;
                devices_[d.id] = d;
                changed.push_back(d);
            } else if (it->second.connected != d.connected) {
                it->second.connected = d.connected;
                changed.push_back(d);
            }
        }
    }
    auto& bus = EventBus::Instance();
    for (const auto& d : changed) {
        if (d.connected) {
            (void)bus.Publish(events::DisplayDeviceConnected{d.id, d.name});
            Logger::Instance().Info("Display connected: " + d.name + " (" + d.id + ")",
                                    "DisplayEngine");
        } else {
            (void)bus.Publish(events::DisplayDeviceDisconnected{d.id});
            Logger::Instance().Info("Display disconnected: " + d.id, "DisplayEngine");
        }
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
Result<void> DisplayEngine::AddOutput(const Output& output) {
    auto r = router_.AddOutput(output);
    if (r.ok() && !output.displayId.empty()) {
        // If the bound device exists and is connected, mark the output ready.
        std::lock_guard<std::mutex> lock(mutex_);
        const DisplayDevice* d = FindDeviceLocked(output.displayId);
        if (d && d->connected) {
            auto out = router_.GetOutput(output.id);
            if (out.ok()) out.value()->state = OutputState::Running;
            (void)EventBus::Instance().Publish(
                events::DisplayReady{output.displayId, "output '" + output.id + "' assigned"});
        }
    }
    return r;
}

Result<void> DisplayEngine::RemoveOutput(std::string_view id) {
    return router_.RemoveOutput(id);
}

Result<Output> DisplayEngine::GetOutput(std::string_view id) const {
    // OutputRouter::GetOutput is non-const; route through a mutable copy.
    auto out = const_cast<OutputRouter&>(router_).GetOutput(id);
    if (!out.ok()) return out.error();
    return *out.value();
}

std::vector<Output> DisplayEngine::Outputs() const {
    return router_.Outputs();
}

size_t DisplayEngine::OutputCount() const {
    return router_.Count();
}

Result<void> DisplayEngine::AssignOutput(std::string_view outputId, std::string_view deviceId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!FindDeviceLocked(deviceId))
            return Error::Make(Err::Display_DeviceNotFound, "DisplayEngine",
                               "device '" + std::string(deviceId) + "' not found");
    }
    auto out = router_.GetOutput(outputId);
    if (!out.ok()) return out.error();
    out.value()->displayId = std::string(deviceId);
    out.value()->state = OutputState::Starting;
    (void)EventBus::Instance().Publish(
        events::OutputStarted{std::string(outputId), std::string(deviceId),
                              static_cast<int>(out.value()->kind)});
    return Ok();
}

Result<void> DisplayEngine::EnableOutput(std::string_view id, bool enabled) {
    return router_.SetEnabled(id, enabled);
}

Result<void> DisplayEngine::SetOutputTransform(std::string_view id,
                                               const OutputTransform& transform) {
    return router_.SetTransform(id, transform);
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------
Result<void> DisplayEngine::SaveProfile(const DisplayProfile& profile) {
    return profiles_.SaveProfile(profile);
}

Result<void> DisplayEngine::ApplyProfile(std::string_view name) {
    auto profileResult = profiles_.GetProfile(name);
    if (!profileResult.ok()) return profileResult.error();
    const DisplayProfile& profile = profileResult.value();
    // Clear existing outputs, then rebuild from the profile.
    for (const auto& o : router_.Outputs()) (void)router_.RemoveOutput(o.id);
    for (const auto& output : profile.outputs) {
        auto r = router_.AddOutput(output);
        if (!r.ok()) {
            errorCount_.fetch_add(1);
            continue;
        }
        if (!output.displayId.empty()) {
            // Re-bind and mark ready when the device is present.
            std::lock_guard<std::mutex> lock(mutex_);
            const DisplayDevice* d = FindDeviceLocked(output.displayId);
            if (d && d->connected) {
                auto out = router_.GetOutput(output.id);
                if (out.ok()) out.value()->state = OutputState::Running;
                (void)EventBus::Instance().Publish(
                    events::OutputStarted{output.id, output.displayId,
                                          static_cast<int>(output.kind)});
            } else {
                auto out = router_.GetOutput(output.id);
                if (out.ok()) out.value()->state = OutputState::Lost;
            }
        }
    }
    (void)EventBus::Instance().Publish(
        events::DisplayProfileApplied{std::string(name), router_.Count()});
    Logger::Instance().Info(std::format("Display profile '{}' applied ({} outputs)", name,
                                        router_.Count()),
                            "DisplayEngine");
    return Ok();
}

Result<DisplayProfile> DisplayEngine::GetProfile(std::string_view name) const {
    return profiles_.GetProfile(name);
}

std::vector<std::string> DisplayEngine::ProfileNames() const {
    return profiles_.ProfileNames();
}

// ---------------------------------------------------------------------------
// Frame routing
// ---------------------------------------------------------------------------
void DisplayEngine::RouteFrame(const rendering::Frame& frame, const OutputRouter::Sink& sink) {
    auto resolver = [this](std::string_view deviceId) -> const DisplayDevice* {
        std::lock_guard<std::mutex> lock(mutex_);
        return FindDeviceLocked(deviceId);
    };
    router_.RouteFrame(frame, resolver, [&](const Output& output, const rendering::Rect& rect,
                                            const rendering::Frame& scaled) {
        framesRouted_.fetch_add(1);
        if (sink) sink(output, rect, scaled);
    });
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------
Result<size_t> DisplayEngine::RestoreAssignments() {
    size_t restored = 0;
    auto outputs = router_.Outputs();
    for (auto& output : outputs) {
        if (!output.autoRestore || output.displayId.empty() || output.state != OutputState::Lost)
            continue;
        std::lock_guard<std::mutex> lock(mutex_);
        const DisplayDevice* d = FindDeviceLocked(output.displayId);
        if (d && d->connected) {
            auto out = router_.GetOutput(output.id);
            if (out.ok()) {
                out.value()->state = OutputState::Running;
                ++restored;
                (void)EventBus::Instance().Publish(
                    events::DisplayRestored{output.id, output.displayId});
                (void)EventBus::Instance().Publish(
                    events::OutputStarted{output.id, output.displayId,
                                          static_cast<int>(output.kind)});
            }
        }
    }
    restoredCount_.store(restored);
    return restored;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
size_t DisplayEngine::ConnectedDeviceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [_, d] : devices_)
        if (d.connected) ++n;
    return n;
}

size_t DisplayEngine::RunningOutputCount() const {
    size_t n = 0;
    for (const auto& o : router_.Outputs())
        if (o.state == OutputState::Running) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void DisplayEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::MonitorConnected>(
        [this](const events::MonitorConnected& e) { OnMonitorConnected(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::MonitorDisconnected>(
        [this](const events::MonitorDisconnected& e) { OnMonitorDisconnected(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::MonitorResolutionChanged>(
        [this](const events::MonitorResolutionChanged& e) { OnMonitorResolutionChanged(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
    subscriptions_.push_back(bus.Subscribe<events::ResourcePressureChanged>(
        [this](const events::ResourcePressureChanged& e) { OnPressure(e); }, 0));
}

void DisplayEngine::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_)
        if (s.Valid()) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
}

void DisplayEngine::OnMonitorConnected(const events::MonitorConnected& e) {
    (void)RefreshDevices();
    Logger::Instance().Info("Hotplug: monitor connected " + e.monitorId, "DisplayEngine");
    // Re-attach outputs that were waiting for this display.
    (void)RestoreAssignments();
}

void DisplayEngine::OnMonitorDisconnected(const events::MonitorDisconnected& e) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = devices_.find(e.monitorId);
        if (it != devices_.end()) it->second.connected = false;
    }
    Logger::Instance().Info("Hotplug: monitor disconnected " + e.monitorId, "DisplayEngine");
    // Mark bound outputs Lost — presentation keeps running (18 §Display Removal).
    auto outputs = router_.Outputs();
    for (auto& output : outputs) {
        if (output.displayId == e.monitorId) {
            auto out = router_.GetOutput(output.id);
            if (out.ok()) {
                out.value()->state = OutputState::Lost;
                out.value()->framesLost++;
                (void)EventBus::Instance().Publish(
                    events::OutputLost{output.id, output.displayId});
            }
        }
    }
}

void DisplayEngine::OnMonitorResolutionChanged(const events::MonitorResolutionChanged& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(e.monitorId);
    if (it != devices_.end()) {
        // e carries only a detail string; refresh from providers is the robust path.
    }
    (void)RefreshDevices();
}

void DisplayEngine::OnConfigReload(const events::ConfigHotReload&) {
    (void)RefreshDevices();
}

void DisplayEngine::OnPressure(const events::ResourcePressureChanged& e) {
    // Under memory pressure, disable non-essential outputs (preview/thumbnail).
    if (e.to == PressureLevel::High || e.to == PressureLevel::Critical) {
        for (const auto& o : router_.Outputs()) {
            if (o.kind == OutputKind::Preview || o.kind == OutputKind::Thumbnail)
                (void)router_.SetEnabled(o.id, false);
        }
    }
}

void DisplayEngine::HotplugTick() {
    if (!running_.load()) return;
    (void)RefreshDevices();
    (void)RestoreAssignments();
}

DisplayTestReport DisplayEngine::RunSelfTest() {
    auto resolver = [this](std::string_view deviceId) -> const DisplayDevice* {
        std::lock_guard<std::mutex> lock(mutex_);
        return FindDeviceLocked(deviceId);
    };
    auto outputs = [this]() { return router_.Outputs(); };
    auto restore = [this]() { return RestoreAssignments(); };
    return DisplayTester::RunAll(resolver, outputs, &router_, restore);
}

} // namespace bps::display
