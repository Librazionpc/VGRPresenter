#pragma once

// DisplayEngine (docs/specs/18): the Phase 7 facade. Owns the provider
// registry (IDisplayProvider), the device view, the output router, the profile
// store and the display/output state machine. Routes one rendered frame to any
// number of outputs; handles hot-plug detection, display removal recovery and
// automatic assignment restore. Knows nothing about WinUI/HWND/XAML.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "interfaces/IService.hpp"
#include "modules/display/DisplayProfiles.hpp"
#include "modules/display/DisplayTest.hpp"
#include "modules/display/IDisplayProvider.hpp"
#include "modules/display/OutputRouter.hpp"
#include "modules/display/Providers.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::display {

class DisplayEngine final : public IService {
public:
    static DisplayEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "DisplayEngine"; }

    // --- Provider registry (18 §Provider-based display model) ---
    Result<void> RegisterProvider(std::shared_ptr<IDisplayProvider> provider);
    Result<void> UnregisterProvider(std::string_view name);
    std::vector<std::string> ProviderNames() const;

    // --- Device view (union across providers, refreshed by Probe) ---
    std::vector<DisplayDevice> Devices() const;
    Result<DisplayDevice> GetDevice(std::string_view id) const;
    // Hot-plug refresh: probes every provider and publishes
    // display.device_connected / display.device_disconnected events for changes.
    Result<void> RefreshDevices();

    // --- Outputs (18 §Output System) ---
    Result<void> AddOutput(const Output& output);
    Result<void> RemoveOutput(std::string_view id);
    Result<Output> GetOutput(std::string_view id) const;
    std::vector<Output> Outputs() const;
    size_t OutputCount() const;
    // Bind a logical output to a device (assign audience -> projector).
    Result<void> AssignOutput(std::string_view outputId, std::string_view deviceId);
    Result<void> EnableOutput(std::string_view id, bool enabled);
    Result<void> SetOutputTransform(std::string_view id, const OutputTransform& transform);

    // --- Profiles (18 §Display/Output Profiles) ---
    Result<void> SaveProfile(const DisplayProfile& profile);
    Result<void> ApplyProfile(std::string_view name);
    Result<DisplayProfile> GetProfile(std::string_view name) const;
    std::vector<std::string> ProfileNames() const;

    // --- Frame routing (18 §One rendered frame) ---
    // Routes ONE frame to all enabled outputs bound to connected devices.
    // `sink` receives each (output, destRect, scaledFrame) for delivery.
    void RouteFrame(const rendering::Frame& frame,
                    const OutputRouter::Sink& sink = nullptr);

    // --- Recovery (18 §Display Removal) ---
    // Re-attaches outputs marked autoRestore whose device came back.
    Result<size_t> RestoreAssignments();

    // --- Diagnostics ---
    size_t ConnectedDeviceCount() const;
    size_t RunningOutputCount() const;
    std::string ProfileDir() const { return profileDir_; }

    // --- Self-test (docs/specs/18 §Testing) ---
    // Runs the full display self-test against the live engine: device
    // connectivity, scaling math, frame delivery to every output, recovery.
    DisplayTestReport RunSelfTest();

private:
    DisplayEngine() = default;

    void WireEvents();
    void UnwireEvents();
    void OnMonitorConnected(const events::MonitorConnected& e);
    void OnMonitorDisconnected(const events::MonitorDisconnected& e);
    void OnMonitorResolutionChanged(const events::MonitorResolutionChanged& e);
    void OnConfigReload(const events::ConfigHotReload& e);
    void OnPressure(const events::ResourcePressureChanged& e);
    void HotplugTick();   // scheduled by Start()

    // Find a device across providers without locking (caller holds lock).
    const DisplayDevice* FindDeviceLocked(std::string_view id) const;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IDisplayProvider>> providers_;
    std::map<std::string, DisplayDevice, std::less<>> devices_;
    OutputRouter router_;
    DisplayProfileStore profiles_;
    std::string profileDir_;
    std::vector<Subscription> subscriptions_;
    std::atomic<TaskId> hotplugTask_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> framesRouted_{0};
    std::atomic<size_t> restoredCount_{0};
};

} // namespace bps::display
