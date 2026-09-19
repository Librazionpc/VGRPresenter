#pragma once

// ProductionEngine (docs/specs/27): the Phase 15 facade. Owns the production
// graph (sources → processing → virtual sources → buses → outputs), routing,
// mixing, planning, meters, ducking, clocks, control signals, snapshots and
// health. It coordinates — it never renders pixels, plays audio, or routes
// screens itself (Rendering/Media/Display engines own those).

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/production/ProductionGraph.hpp"
#include "modules/production/ProductionTypes.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::production {

// A recorded macro step (docs/specs/27 §Macro Recording).
struct MacroStep {
    std::string action;   // e.g. "set_scene_state", "set_volume", "assign_outputs"
    std::string payload;  // JSON-ish string
};

class ProductionEngine final : public IService {
public:
    static ProductionEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "ProductionEngine"; }

    // --- Graph access ---
    // NOTE: Graph() returns the live graph. The facade's methods serialize via
    // mutex_; direct Graph() mutation is intended for single-threaded
    // configuration/automation use and is not synchronized (docs/specs/27).
    ProductionGraph& Graph() { return graph_; }
    const ProductionGraph& Graph() const { return graph_; }

    // --- Bus / output operations (docs/specs/27 §Bus hierarchy, §Outputs) ---
    Result<std::string> CreateBus(std::string_view id, std::string_view name,
                                  BusRole role, SignalType type);
    Result<void> SaveBusScene(std::string_view busId, std::string_view sceneName);
    Result<void> ApplyBusScene(std::string_view busId, std::string_view sceneName);
    Result<void> AssignOutputBuses(std::string_view outputId, std::string_view videoBus,
                                   std::string_view audioBus);
    Result<void> SetOutputPriority(std::string_view outputId, OutputPriority p);

    // --- Virtual sources (docs/specs/27 §Virtual signals) ---
    // Registers a bus as a routable virtual source and returns its node id.
    Result<std::string> CreateVirtualSource(std::string_view busId);

    // --- Mixing (docs/specs/27 §Volume, §Processing, §Meters, §Ducking) ---
    Result<void> SetGain(std::string_view nodeId, double gainDb);
    Result<void> SetMute(std::string_view nodeId, bool mute);
    Result<void> SetSolo(std::string_view nodeId, bool solo);
    Result<void> SetPan(std::string_view nodeId, double pan);
    Result<void> AddProcessing(std::string_view nodeId, const ProcessingStage& stage);
    Result<MeterLevels> GetMeters(std::string_view nodeId) const;
    Result<void> UpdateMeters(std::string_view nodeId, const MeterLevels& m);
    // Ducking: while `triggerSource` is active, `targetBus` is attenuated by
    // `depthDb`; `ReleaseDuck` restores it (user brief §13).
    Result<void> Duck(std::string_view targetBus, std::string_view triggerSource,
                      double depthDb);
    Result<void> ReleaseDuck(std::string_view targetBus, std::string_view triggerSource);
    // Sum of active duck depths applied to a bus (0 when none).
    double DuckDepth(std::string_view targetBus) const;

    // --- Fallback + health (docs/specs/27 §Scenes, Health, Fallback) ---
    Result<void> SetSourceFallback(std::string_view sourceId, std::string_view fallbackId);
    Result<void> TriggerFallback(std::string_view sourceId, std::string_view reason);
    Result<void> SetSceneState(std::string_view sceneId, SceneState state);

    // --- Clock (docs/specs/27 §Clock) ---
    ClockSnapshot Clock() const;
    void TickClock(int64_t advanceMs);               // advances the master clock
    Result<void> SetClockOffset(std::string_view domain, int64_t offsetMs);
    Result<void> SetClockSync(bool synced);

    // --- Control signals (docs/specs/27 §Control Signals) ---
    // Sends a first-class control signal; any connected Control nodes consume it.
    Result<void> SendControl(std::string_view kind, std::string_view payload);
    size_t ControlSignalCount() const { return controlCount_.load(); }

    // --- Macro recording (docs/specs/27 §Macro Recording) ---
    Result<void> StartMacroRecording(std::string_view name);
    Result<std::vector<MacroStep>> StopMacroRecording();
    std::vector<std::string> MacroNames() const;

    // --- Planning + validation + simulation (docs/specs/27 §Resources) ---
    // Analyzes the graph and computes the total declared resource cost.
    ProductionPlan PlanProduction(const ResourceCost& budget) const;
    std::vector<ValidationIssue> ValidateProduction() const;
    // One-click production check (user brief §46).
    ProductionHealth CheckProduction() const;
    // Dry-run: initialize the graph for validation without going live.
    Result<void> Simulate();
    // Frame-accurate cue: execute at a master-clock position (user brief §25).
    Result<void> ScheduleCue(int64_t atMs, std::string_view action, std::string_view payload);
    size_t CueCount() const { return cueCount_.load(); }

    // --- Snapshots + emergency (docs/specs/27 §Snapshots, §Emergency) ---
    Result<std::string> SaveProductionSnapshot(std::string_view name);
    Result<void> RestoreProductionSnapshot(std::string_view name);
    Result<void> EmergencyMode(std::string_view reason);

    // --- Atomic live changes (docs/specs/27 §Atomic live changes) ---
    Result<void> BeginEdit();
    Result<void> CommitEdit();
    void RollbackEdit();

    // --- AI-ready validated commands (docs/specs/27 §AI-Readiness) ---
    // The single safe mutation path: parse → validate → apply.
    Result<void> ApplyCommand(std::string_view command);

    // --- Events ---
    void WireEvents();
    void UnwireEvents();
    void OnDisplayConnected(const events::DisplayDeviceConnected& e);
    void OnDisplayDisconnected(const events::DisplayDeviceDisconnected& e);
    int DisplaysConnected() const { return displaysConnected_.load(); }

private:
    ProductionEngine() = default;

    void Record(const std::string& action, const std::string& payload);

    ProductionGraph graph_;
    std::map<std::string, MeterLevels, std::less<>> meters_;
    // duck targetBus+triggerSource -> depthDb
    std::map<std::string, double, std::less<>> ducks_;
    std::map<std::string, std::vector<std::string>, std::less<>> scheduledCues_;  // atMs -> actions
    std::map<std::string, std::vector<MacroStep>, std::less<>> macros_;
    std::string recordingName_;
    std::vector<MacroStep> recording_;
    std::vector<Subscription> subscriptions_;
    ClockSnapshot clock_;
    int64_t audioOffsetMs_ = 0;   // persistent domain offsets (docs/specs/27 §Clock)
    int64_t videoOffsetMs_ = 0;
    int64_t networkOffsetMs_ = 0;
    std::atomic<uint64_t> controlCount_{0};
    std::atomic<uint64_t> cueCount_{0};
    std::atomic<int> displaysConnected_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    mutable std::recursive_mutex mutex_;
};

} // namespace bps::production
