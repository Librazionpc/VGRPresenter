#include "modules/production/ProductionEngine.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <sstream>

namespace bps::production {

namespace {
constexpr const char* kModule = "ProductionEngine";

std::vector<std::string> Split(std::string_view s) {
    std::vector<std::string> out;
    std::istringstream ss{std::string(s)};
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

std::string Join(const std::vector<std::string>& parts, size_t from) {
    std::string out;
    for (size_t i = from; i < parts.size(); ++i) {
        if (i > from) out += " ";
        out += parts[i];
    }
    return out;
}

// Exception-free numeric parse (00 §1: no exceptions across the Result
// boundary — std::stod would throw on malformed AI/user input).
bool ToDouble(std::string_view s, double& out) {
    std::string tmp(s);
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

std::string DuckKey(std::string_view bus, std::string_view trigger) {
    return std::string(bus) + "\x1f" + std::string(trigger);
}
} // namespace

ProductionEngine& ProductionEngine::Instance() {
    static ProductionEngine inst;
    return inst;
}

// --- Lifecycle ---

Result<void> ProductionEngine::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    WireEvents();

    // --- Graph persistence (kernel owns the state, the DB owns the bytes) ---
    // Restore the last saved graph, then keep it saved: every successful
    // graph mutation re-serializes through the saver into the kernel's
    // DatabaseManager (the DB flushes to disk at its own shutdown, and the
    // engine flushes a final save at its Shutdown).
    auto& db = DatabaseManager::Instance();
    if (auto doc = db.Get("production", "graph"); doc.ok() && !doc.value().isNull()) {
        if (auto r = graph_.Restore(doc.value()); r.ok())
            Logger::Instance().Info("production graph restored (" +
                                        std::to_string(graph_.NodeCount()) + " nodes)",
                                    kModule);
        else
            Logger::Instance().Warning("production graph restore failed: " +
                                           r.error().message,
                                       kModule);
    }
    graph_.InstallSaver([](json::Value doc) {
        auto& db2 = DatabaseManager::Instance();
        (void)db2.Put("production", "graph", std::move(doc));
        // Durable per mutation: the flush writes the DB file, so even a hard
        // process kill only loses the mutation in flight. Graph edits are
        // user-click paced — a small write each time is the right trade.
        (void)db2.Flush();
    });
    return Ok();
}

Result<void> ProductionEngine::Start() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_.load()) return Error::Make(Err::Production_InvalidState, kModule,
                                                 "Initialize() first");
    if (running_.load()) return Ok();
    running_.store(true);
    (void)EventBus::Instance().Publish(events::ProductionStarted{});
    return Ok();
}

Result<void> ProductionEngine::Stop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_.load()) return Ok();
    running_.store(false);
    (void)EventBus::Instance().Publish(events::ProductionStopped{});
    return Ok();
}

Result<void> ProductionEngine::Shutdown() {
    UnwireEvents();
    // Final save before the DatabaseManager flushes (kernel tears services
    // down in registration order — production before database — so this is
    // the last chance to capture any tail mutations).
    (void)graph_.PersistDirty();
    initialized_.store(false);
    running_.store(false);
    return Ok();
}

Result<void> ProductionEngine::Reload() {
    // Re-validate the current graph; keep state (engines are live systems).
    if (auto v = ValidateProduction(); !v.empty())
        for (const auto& i : v)
            if (i.severity == ValidationIssue::Severity::Error)
                Logger::Instance().Warning("production reload: " + i.message, kModule);
    return Ok();
}

Result<void> ProductionEngine::Reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    meters_.clear();
    ducks_.clear();
    scheduledCues_.clear();
    clock_ = ClockSnapshot{};
    audioOffsetMs_ = 0;
    videoOffsetMs_ = 0;
    networkOffsetMs_ = 0;
    return Ok();
}    HealthReport ProductionEngine::GetHealth() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto issues = graph_.Validate();
    size_t errs = 0, warns = 0;
    for (const auto& i : issues) {
        if (i.severity == ValidationIssue::Severity::Error) ++errs;
        else ++warns;
    }
    HealthReport hr;
    if (errs > 0) {
        hr.state = HealthState::Degraded;
        hr.detail = std::format("{} validation errors", errs);
    } else if (warns > 0) {
        hr.detail = std::format("{} warnings", warns);
    } else if (!running_.load()) {
        hr.detail = "initialized, not running";
    } else {
        hr.detail = std::format("routing {} nodes", graph_.NodeCount());
    }
    return hr;
}

Metrics ProductionEngine::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    m.queueLength = graph_.NodeCount();
    m.health = running_.load() ? HealthState::Healthy : HealthState::Degraded;
    return m;
}

// --- Buses / outputs ---

Result<std::string> ProductionEngine::CreateBus(std::string_view id, std::string_view name,
                                                BusRole role, SignalType type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.AddBus(id, name, role, type);
    if (r.ok()) Record("create_bus", std::string(r.value()));
    return r;
}

Result<void> ProductionEngine::SaveBusScene(std::string_view busId,
                                            std::string_view sceneName) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.SaveBusScene(busId, sceneName);
    if (r.ok()) Record("save_bus_scene", std::string(busId) + " " + std::string(sceneName));
    return r;
}

Result<void> ProductionEngine::ApplyBusScene(std::string_view busId,
                                             std::string_view sceneName) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.ApplyBusScene(busId, sceneName);
    if (r.ok()) {
        Record("apply_bus_scene", std::string(busId) + " " + std::string(sceneName));
        (void)EventBus::Instance().Publish(
            events::BusSceneApplied{std::string(busId), std::string(sceneName)});
    }
    return r;
}

Result<void> ProductionEngine::AssignOutputBuses(std::string_view outputId,
                                                 std::string_view videoBus,
                                                 std::string_view audioBus) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.AssignOutputBuses(outputId, videoBus, audioBus);
    if (r.ok()) {
        Record("assign_outputs", std::string(outputId) + " " + std::string(videoBus) +
                                     " " + std::string(audioBus));
        (void)EventBus::Instance().Publish(events::OutputChanged{
            std::string(outputId), std::string(videoBus), std::string(audioBus)});
    }
    return r;
}

Result<void> ProductionEngine::SetOutputPriority(std::string_view outputId,
                                                 OutputPriority p) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.SetOutputPriority(outputId, p);
    if (r.ok()) Record("set_output_priority",
                       std::string(outputId) + " " + ToString(p));
    return r;
}

// --- Virtual sources ---

Result<std::string> ProductionEngine::CreateVirtualSource(std::string_view busId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto bus = graph_.GetNode(busId);
    if (!bus.ok() || bus.value().kind != NodeKind::Bus)
        return Error::Make(Err::Production_BusNotFound, kModule,
                           "no bus: " + std::string(busId));
    auto vs = graph_.AddVirtualSource("", std::string(busId) + " (virtual)",
                                      bus.value().signalType);
    if (!vs.ok()) return vs.error();
    // The virtual source consumes the bus output.
    if (auto c = graph_.Connect(busId, vs.value(), bus.value().signalType); !c.ok())
        return c.error();
    Record("create_virtual_source", std::string(busId) + " " + vs.value());
    (void)EventBus::Instance().Publish(
        events::VirtualSourceCreated{std::string(busId), vs.value()});
    return vs;
}

// --- Mixing ---

Result<void> ProductionEngine::SetGain(std::string_view nodeId, double gainDb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto v = graph_.Volume(nodeId);
    if (!v.ok()) return v.error();
    v.value().gainDb = gainDb;
    auto r = graph_.SetVolume(nodeId, v.value());
    if (r.ok()) Record("set_gain", std::format("{} {}", nodeId, gainDb));
    return r;
}

Result<void> ProductionEngine::SetMute(std::string_view nodeId, bool mute) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto v = graph_.Volume(nodeId);
    if (!v.ok()) return v.error();
    v.value().mute = mute;
    auto r = graph_.SetVolume(nodeId, v.value());
    if (r.ok()) Record("set_mute", std::format("{} {}", nodeId, mute));
    return r;
}

Result<void> ProductionEngine::SetSolo(std::string_view nodeId, bool solo) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto v = graph_.Volume(nodeId);
    if (!v.ok()) return v.error();
    v.value().solo = solo;
    auto r = graph_.SetVolume(nodeId, v.value());
    if (r.ok()) Record("set_solo", std::format("{} {}", nodeId, solo));
    return r;
}

Result<void> ProductionEngine::SetPan(std::string_view nodeId, double pan) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto v = graph_.Volume(nodeId);
    if (!v.ok()) return v.error();
    v.value().pan = pan;
    auto r = graph_.SetVolume(nodeId, v.value());
    if (r.ok()) Record("set_pan", std::format("{} {}", nodeId, pan));
    return r;
}

Result<void> ProductionEngine::AddProcessing(std::string_view nodeId,
                                             const ProcessingStage& stage) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.AddProcessing(nodeId, stage);
    if (r.ok()) Record("add_processing", std::string(nodeId));
    return r;
}

Result<MeterLevels> ProductionEngine::GetMeters(std::string_view nodeId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!graph_.HasNode(nodeId))
        return Error::Make(Err::Production_NodeNotFound, kModule,
                           "no node: " + std::string(nodeId));
    auto it = meters_.find(std::string(nodeId));
    if (it == meters_.end()) {
        MeterLevels m;
        m.headroom = 18.0;  // healthy default
        return m;
    }
    return it->second;
}

Result<void> ProductionEngine::UpdateMeters(std::string_view nodeId, const MeterLevels& m) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!graph_.HasNode(nodeId))
        return Error::Make(Err::Production_NodeNotFound, kModule,
                           "no node: " + std::string(nodeId));
    meters_[std::string(nodeId)] = m;
    return Ok();
}

Result<void> ProductionEngine::Duck(std::string_view targetBus,
                                    std::string_view triggerSource, double depthDb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto bus = graph_.GetNode(targetBus);
    if (!bus.ok() || bus.value().kind != NodeKind::Bus)
        return Error::Make(Err::Production_BusNotFound, kModule,
                           "no bus: " + std::string(targetBus));
    if (!graph_.HasNode(triggerSource))
        return Error::Make(Err::Production_SourceNotFound, kModule,
                           "no source: " + std::string(triggerSource));
    ducks_[DuckKey(targetBus, triggerSource)] = depthDb;
    Record("duck", std::format("{} {} {}", targetBus, triggerSource, depthDb));
    return Ok();
}

Result<void> ProductionEngine::ReleaseDuck(std::string_view targetBus,
                                           std::string_view triggerSource) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = ducks_.find(DuckKey(targetBus, triggerSource));
    if (it == ducks_.end())
        return Error::Make(Err::Production_NodeNotFound, kModule,
                           "no active duck on " + std::string(targetBus));
    ducks_.erase(it);
    Record("release_duck", std::string(targetBus) + " " + std::string(triggerSource));
    return Ok();
}

double ProductionEngine::DuckDepth(std::string_view targetBus) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    double depth = 0.0;
    std::string prefix = std::string(targetBus) + "\x1f";
    for (const auto& [k, d] : ducks_)
        if (k.starts_with(prefix)) depth += d;
    return depth;
}

// --- Fallback + scenes ---

Result<void> ProductionEngine::SetSourceFallback(std::string_view sourceId,
                                                 std::string_view fallbackId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.SetFallback(sourceId, fallbackId);
    if (r.ok()) Record("set_fallback", std::string(sourceId) + " " +
                                           std::string(fallbackId));
    return r;
}

Result<void> ProductionEngine::TriggerFallback(std::string_view sourceId,
                                               std::string_view reason) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto node = graph_.GetNode(sourceId);
    if (!node.ok()) return node.error();
    if (node.value().kind != NodeKind::Source)
        return Error::Make(Err::Production_InvalidState, kModule,
                           "not a source: " + std::string(sourceId));
    // Mark the primary source failed.
    (void)graph_.SetSourceState(sourceId, SourceState::Failed);
    (void)graph_.SetEnabled(sourceId, false);
    (void)EventBus::Instance().Publish(
        events::SourceFailed{std::string(sourceId), std::string(reason)});
    if (!node.value().fallbackId.empty()) {
        const auto& fb = node.value().fallbackId;
        (void)graph_.SetSourceState(fb, SourceState::Stable);
        (void)graph_.SetEnabled(fb, true);
        (void)EventBus::Instance().Publish(
            events::SourceFallback{std::string(sourceId), fb});
    }
    Record("fallback", std::string(sourceId) + " " + std::string(reason));
    return Ok();
}

Result<void> ProductionEngine::SetSceneState(std::string_view sceneId, SceneState state) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.SetSceneState(sceneId, state);
    if (r.ok()) {
        Record("set_scene_state", std::string(sceneId) + " " + ToString(state));
        (void)EventBus::Instance().Publish(
            events::SceneStateChanged{std::string(sceneId), ToString(state)});
    }
    return r;
}

// --- Clock ---

ClockSnapshot ProductionEngine::Clock() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return clock_;
}

void ProductionEngine::TickClock(int64_t advanceMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    clock_.masterMs += advanceMs;
    clock_.audioMs = clock_.masterMs + audioOffsetMs_;
    clock_.videoMs = clock_.masterMs + videoOffsetMs_;
    clock_.networkMs = clock_.masterMs + networkOffsetMs_;
    // Fire any cues whose time has come (frame-accurate, docs/specs/27 §Cues).
    std::vector<std::string> due;
    for (auto it = scheduledCues_.begin(); it != scheduledCues_.end();) {
        if (std::stoll(it->first) <= clock_.masterMs) {
            due = it->second;
            it = scheduledCues_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& step : due) {
        auto parts = Split(step);
        if (!parts.empty() && parts[0] == "control" && parts.size() >= 3)
            (void)SendControl(parts[1], Join(parts, 2));
    }
}

Result<void> ProductionEngine::SetClockOffset(std::string_view domain, int64_t offsetMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (domain == "audio") audioOffsetMs_ = offsetMs;
    else if (domain == "video") videoOffsetMs_ = offsetMs;
    else if (domain == "network") networkOffsetMs_ = offsetMs;
    else return Error::Make(Err::Production_InvalidState, kModule,
                            "unknown clock domain: " + std::string(domain));
    clock_.audioMs = clock_.masterMs + audioOffsetMs_;
    clock_.videoMs = clock_.masterMs + videoOffsetMs_;
    clock_.networkMs = clock_.masterMs + networkOffsetMs_;
    return Ok();
}

Result<void> ProductionEngine::SetClockSync(bool synced) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (clock_.synced == synced) return Ok();
    clock_.synced = synced;
    (void)EventBus::Instance().Publish(events::ClockSyncChanged{synced});
    return Ok();
}

// --- Control signals ---

Result<void> ProductionEngine::SendControl(std::string_view kind, std::string_view payload) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    controlCount_.fetch_add(1);
    Record("send_control", std::string(kind) + " " + std::string(payload));
    (void)EventBus::Instance().Publish(
        events::ControlSignalReceived{std::string(kind), std::string(payload)});
    return Ok();
}

// --- Macro recording ---

Result<void> ProductionEngine::StartMacroRecording(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!recordingName_.empty())
        return Error::Make(Err::Production_InvalidState, kModule,
                           "already recording '" + recordingName_ + "'");
    recordingName_ = std::string(name);
    recording_.clear();
    return Ok();
}

Result<std::vector<MacroStep>> ProductionEngine::StopMacroRecording() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (recordingName_.empty())
        return Error::Make(Err::Production_InvalidState, kModule, "not recording");
    macros_[recordingName_] = recording_;
    std::string name = recordingName_;
    recordingName_.clear();
    recording_.clear();
    return macros_[name];
}

std::vector<std::string> ProductionEngine::MacroNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, steps] : macros_) out.push_back(name);
    return out;
}

void ProductionEngine::Record(const std::string& action, const std::string& payload) {
    if (!recordingName_.empty()) recording_.push_back({action, payload});
}

// --- Planning / validation / health ---

ProductionPlan ProductionEngine::PlanProduction(const ResourceCost& budget) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ProductionPlan plan;
    plan.budget = budget;
    std::vector<std::pair<std::string, ResourceCost>> perNode;
    ResourceCost total;
    for (const auto& id : graph_.TopologicalOrder()) {
        auto n = graph_.GetNode(id);
        if (!n.ok() || !n.value().enabled) continue;
        total += n.value().cost;
        perNode.emplace_back(id, n.value().cost);
    }
    plan.total = total;
    plan.perNode = std::move(perNode);
    // Bottleneck = dimension furthest over (or closest to) budget.
    auto ratio = [&](int used, int budgeted) {
        return budgeted > 0 ? static_cast<double>(used) / static_cast<double>(budgeted) : 0.0;
    };
    double rg = ratio(total.gpu, budget.gpu);
    double rc = ratio(total.cpu, budget.cpu);
    double rv = budget.vramMb > 0 ? static_cast<double>(total.vramMb) /
                                        static_cast<double>(budget.vramMb) : 0.0;
    double rr = budget.ramMb > 0 ? static_cast<double>(total.ramMb) /
                                        static_cast<double>(budget.ramMb) : 0.0;
    double worst = std::max({rg, rc, rv, rr});
    if (rg == worst) plan.bottleneck = "gpu";
    else if (rc == worst) plan.bottleneck = "cpu";
    else if (rv == worst) plan.bottleneck = "vram";
    else plan.bottleneck = "ram";
    plan.feasible = worst <= 1.0;
    // Encoder selection: GPU-heavy work prefers hardware encode.
    plan.encoder = (total.gpu > budget.gpu / 2) ? "hardware" : "software";
    (void)EventBus::Instance().Publish(
        events::ProductionPlanned{plan.feasible, plan.bottleneck});
    return plan;
}

std::vector<ValidationIssue> ProductionEngine::ValidateProduction() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto issues = graph_.Validate();
    size_t errs = 0, warns = 0;
    for (const auto& i : issues) {
        if (i.severity == ValidationIssue::Severity::Error) ++errs;
        else ++warns;
    }
    (void)EventBus::Instance().Publish(events::ProductionValidated{errs, warns});
    return issues;
}

ProductionHealth ProductionEngine::CheckProduction() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ProductionHealth h;
    auto issues = graph_.Validate();
    size_t errs = 0, warns = 0;
    for (const auto& i : issues) {
        if (i.severity == ValidationIssue::Severity::Error) ++errs;
        else ++warns;
    }
    // Subsystem scores.
    auto sources = graph_.SourceIds();
    int srcScore = 100;
    for (const auto& s : sources) {
        auto n = graph_.GetNode(s);
        if (n.ok() && (n.value().sourceState == SourceState::Failed ||
                       n.value().sourceState == SourceState::Disconnected))
            srcScore = std::max(0, srcScore - 25);
    }
    auto buses = graph_.BusIds();
    int busScore = 100;
    for (const auto& b : buses) {
        auto n = graph_.GetNode(b);
        if (n.ok() && graph_.Upstream(b).empty()) busScore = std::max(0, busScore - 10);
    }
    auto outputs = graph_.OutputIds();
    int outScore = 100;
    for (const auto& o : outputs) {
        auto c = graph_.GetOutputConfig(o);
        if (c.ok() && c.value().audioBusId.empty() && c.value().videoBusId.empty())
            outScore = std::max(0, outScore - 20);
    }
    int routeScore = std::max(0, 100 - static_cast<int>(errs) * 30);
    int score = static_cast<int>((srcScore + busScore + outScore + routeScore) / 4.0);
    if (warns > 0) score = std::max(0, score - 5);
    h.score = score;
    h.checks = {{"sources", srcScore}, {"buses", busScore},
                {"outputs", outScore}, {"routing", routeScore}};
    h.message = errs == 0 ? (warns == 0 ? "Production Ready" : "Ready with warnings")
                          : "Issues found";
    return h;
}

Result<void> ProductionEngine::Simulate() {
    // Dry-run: validate and plan without going live.
    auto issues = ValidateProduction();
    for (const auto& i : issues)
        if (i.severity == ValidationIssue::Severity::Error)
            return Error::Make(Err::Production_ValidationFailed, kModule, i.message);
    ResourceCost budget{100, 100, 8192, 16384};
    (void)PlanProduction(budget);
    return Ok();
}

Result<void> ProductionEngine::ScheduleCue(int64_t atMs, std::string_view action,
                                           std::string_view payload) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string step = std::string(action);
    if (!payload.empty()) step += " " + std::string(payload);
    scheduledCues_[std::to_string(atMs)].push_back(step);
    cueCount_.fetch_add(1);
    return Ok();
}

// --- Snapshots + emergency ---

Result<std::string> ProductionEngine::SaveProductionSnapshot(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.SaveSnapshot(name);
    if (r.ok()) {
        Record("save_snapshot", std::string(name));
        auto snap = graph_.GetSnapshot(name);
        (void)EventBus::Instance().Publish(events::ProductionSnapshotSaved{
            std::string(name), snap.ok() ? snap.value().buses.size() : 0});
    }
    return r;
}

Result<void> ProductionEngine::RestoreProductionSnapshot(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto r = graph_.RestoreSnapshot(name);
    if (r.ok()) {
        Record("restore_snapshot", std::string(name));
        (void)EventBus::Instance().Publish(
            events::ProductionRestored{std::string(name)});
    }
    return r;
}

Result<void> ProductionEngine::EmergencyMode(std::string_view reason) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // One-button fallback: emergency scene state, mute non-critical sources,
    // keep outputs (stream stays up) — user brief §50.
    for (const auto& id : graph_.SceneIds()) {
        auto n = graph_.GetNode(id);
        if (n.ok() && n.value().sceneState == SceneState::Program) {
            (void)graph_.SetSceneState(id, SceneState::Emergency);
            (void)EventBus::Instance().Publish(
                events::SceneStateChanged{id, ToString(SceneState::Emergency)});
        }
    }
    for (const auto& id : graph_.SourceIds()) {
        auto n = graph_.GetNode(id);
        if (!n.ok()) continue;
        // Keep critical sources; mute the rest.
        auto v = n.value().volume;
        if (n.value().cost.gpu < 40) v.mute = true;
        (void)graph_.SetVolume(id, v);
    }
    Record("emergency", std::string(reason));
    (void)EventBus::Instance().Publish(events::ProductionEmergency{std::string(reason)});
    return Ok();
}

// --- Atomic live changes ---

Result<void> ProductionEngine::BeginEdit() { return graph_.BeginEdit(); }
Result<void> ProductionEngine::CommitEdit() { return graph_.CommitEdit(); }
void ProductionEngine::RollbackEdit() { graph_.RollbackEdit(); }

// --- AI-ready validated commands ---

Result<void> ProductionEngine::ApplyCommand(std::string_view command) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto parts = Split(command);
    if (parts.empty()) return Error::Make(Err::Production_InvalidState, kModule,
                                          "empty command");
    const std::string verb = parts[0];

    auto need = [&](size_t n) -> Result<void> {
        if (parts.size() < n)
            return Error::Make(Err::Production_InvalidState, kModule,
                               std::format("command '{}' needs {}", verb, n) +
                                   " args");
        return Ok();
    };

    if (verb == "set_scene" && need(3).ok()) {
        auto state = parts[2] == "program"    ? SceneState::Program
                     : parts[2] == "preview"  ? SceneState::Preview
                     : parts[2] == "standby"  ? SceneState::Standby
                     : parts[2] == "disabled" ? SceneState::Disabled
                     : parts[2] == "emergency" ? SceneState::Emergency
                                               : SceneState::Standby;
        if (!graph_.HasNode(parts[1]))
            return Error::Make(Err::Production_SceneNotFound, kModule,
                               "no scene: " + parts[1]);
        return SetSceneState(parts[1], state);
    }
    if (verb == "mute" && need(3).ok()) {
        if (!graph_.HasNode(parts[1]))
            return Error::Make(Err::Production_NodeNotFound, kModule,
                               "no node: " + parts[1]);
        return SetMute(parts[1], parts[2] == "1" || parts[2] == "true");
    }
    if (verb == "gain" && need(3).ok()) {
        if (!graph_.HasNode(parts[1]))
            return Error::Make(Err::Production_NodeNotFound, kModule,
                               "no node: " + parts[1]);
        double db = 0.0;
        if (!ToDouble(parts[2], db))
            return Error::Make(Err::Production_InvalidState, kModule,
                               "invalid gain value: " + parts[2]);
        return SetGain(parts[1], db);
    }
    if (verb == "assign" && need(4).ok()) {
        if (!graph_.HasNode(parts[1]))
            return Error::Make(Err::Production_OutputNotFound, kModule,
                               "no output: " + parts[1]);
        return AssignOutputBuses(parts[1], parts[2], parts[3]);
    }
    if (verb == "duck" && need(4).ok()) {
        double depth = 0.0;
        if (!ToDouble(parts[3], depth))
            return Error::Make(Err::Production_InvalidState, kModule,
                               "invalid duck depth: " + parts[3]);
        return Duck(parts[1], parts[2], depth);
    }
    if (verb == "release_duck" && need(3).ok()) return ReleaseDuck(parts[1], parts[2]);
    if (verb == "fallback" && need(3).ok()) return TriggerFallback(parts[1], Join(parts, 2));
    if (verb == "send_control" && need(3).ok()) return SendControl(parts[1], Join(parts, 2));
    return Error::Make(Err::Production_InvalidState, kModule,
                       "unknown production command: " + verb);
}

// --- Events ---

void ProductionEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::DisplayDeviceConnected>(
        [this](const events::DisplayDeviceConnected& e) { OnDisplayConnected(e); }));
    subscriptions_.push_back(bus.Subscribe<events::DisplayDeviceDisconnected>(
        [this](const events::DisplayDeviceDisconnected& e) { OnDisplayDisconnected(e); }));
}

void ProductionEngine::UnwireEvents() {
    for (const auto& s : subscriptions_) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

void ProductionEngine::OnDisplayConnected(const events::DisplayDeviceConnected&) {
    displaysConnected_.fetch_add(1);
}

void ProductionEngine::OnDisplayDisconnected(const events::DisplayDeviceDisconnected&) {
    displaysConnected_.fetch_sub(1);
}

} // namespace bps::production
