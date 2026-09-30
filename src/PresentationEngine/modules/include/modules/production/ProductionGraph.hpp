#pragma once

// ProductionGraph (docs/specs/27 §Production Graph): the universal node graph
// for the Phase 15 engine. Owns sources, processors, buses, mixers, scenes,
// outputs, virtual sources, data and control nodes, plus the directed routing
// edges between them. Circular routing is rejected at Connect time; bus scenes
// and production snapshots capture/restore state; BeginEdit/CommitEdit/Rollback
// give atomic live changes.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/production/ProductionTypes.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bps::production {

class ProductionGraph {
public:
    // --- Node management ---
    // `id` may be empty to auto-generate; returns the actual id.
    Result<std::string> AddSource(std::string_view id, std::string_view name,
                                  SignalType type, ResourceCost cost = {});
    Result<std::string> AddBus(std::string_view id, std::string_view name,
                               BusRole role, SignalType type);
    Result<std::string> AddProcessor(std::string_view id, std::string_view name,
                                     SignalType type, ResourceCost cost = {});
    Result<std::string> AddMixer(std::string_view id, std::string_view name,
                                 SignalType type);
    Result<std::string> AddScene(std::string_view id, std::string_view name,
                                 SignalType type);
    Result<std::string> AddOutput(std::string_view id, std::string_view name,
                                  SignalType type, const OutputConfig& cfg);
    Result<std::string> AddVirtualSource(std::string_view id, std::string_view name,
                                         SignalType type);
    Result<std::string> AddDataNode(std::string_view id, std::string_view name);
    Result<std::string> AddControlNode(std::string_view id, std::string_view name);
    Result<void> RemoveNode(std::string_view id);

    // --- Routing ---
    // Directed edge from -> to. Rejects circular routes (DFS) and signal
    // mismatches. Data/Control nodes accept any signal type.
    Result<void> Connect(std::string_view from, std::string_view to, SignalType type);
    Result<void> Disconnect(std::string_view from, std::string_view to);
    // THE one validity answer, for optimistic UI (drag-connect affordances)
    // and callers that must not attempt: the same checks Connect() enforces
    // (type compatibility, cycle), plus the VIDEO 1:1 policy — a video bus
    // renders exactly one source, so it is already full unless the edge
    // being tested is the route that exists. Everything Connect() would
    // reject answers false here; one rule, one home.
    bool CanConnect(std::string_view from, std::string_view to, SignalType type) const;
    bool WouldCreateCycle(std::string_view from, std::string_view to) const;
    // Deterministic dependency order (Kahn) used by planner + inspector.
    std::vector<std::string> TopologicalOrder() const;
    std::vector<std::string> Downstream(std::string_view id) const;
    std::vector<std::string> Upstream(std::string_view id) const;

    // --- Node configuration ---
    Result<void> SetEnabled(std::string_view id, bool enabled);
    // Display-name change (rename flows through the graph so every view of
    // the node sees one truth).
    Result<void> RenameNode(std::string_view id, std::string_view name);
    Result<void> SetSourceState(std::string_view id, SourceState state);
    Result<void> SetSceneState(std::string_view id, SceneState state);
    Result<void> SetVolume(std::string_view id, const VolumeControl& vol);
    Result<VolumeControl> Volume(std::string_view id) const;
    // Free-form node metadata (serialized with the graph; see NodeInfo::meta).
    Result<void> SetMeta(std::string_view id, std::string_view key,
                         std::string_view value);
    Result<void> SetLayout(std::string_view id, ChannelLayout layout);
    Result<void> AddProcessing(std::string_view id, const ProcessingStage& stage);
    Result<void> ClearProcessing(std::string_view id);
    Result<void> SetFallback(std::string_view sourceId, std::string_view fallbackId);
    Result<NodeInfo> GetNode(std::string_view id) const;
    bool HasNode(std::string_view id) const;
    std::vector<std::string> NodeIds() const;
    std::vector<std::string> BusIds() const;
    std::vector<std::string> SourceIds() const;
    std::vector<std::string> OutputIds() const;
    std::vector<std::string> SceneIds() const;
    std::vector<std::string> VirtualSourceIds() const;

    // --- Outputs ---
    // Every output is a complete signal destination (video bus + audio bus).
    Result<void> AssignOutputBuses(std::string_view outputId,
                                   std::string_view videoBusId,
                                   std::string_view audioBusId);
    Result<OutputConfig> GetOutputConfig(std::string_view outputId) const;
    Result<void> SetOutputPriority(std::string_view outputId, OutputPriority p);
    Result<void> SetOutputGroup(std::string_view outputId, std::string_view group);
    Result<void> SetOutputFailover(std::string_view outputId,
                                   const std::vector<std::string>& order);
    Result<void> SetOutputEncoder(std::string_view outputId, std::string_view encoder);

    // --- Bus scenes (user brief §5, §6) ---
    Result<void> SaveBusScene(std::string_view busId, std::string_view sceneName);
    Result<void> ApplyBusScene(std::string_view busId, std::string_view sceneName);
    std::vector<std::string> BusSceneNames(std::string_view busId) const;

    // --- Production snapshots (user brief §49) ---
    Result<std::string> SaveSnapshot(std::string_view name);
    Result<void> RestoreSnapshot(std::string_view name);
    std::vector<std::string> SnapshotNames() const;
    Result<ProductionSnapshot> GetSnapshot(std::string_view name) const;

    // --- Validation + inspection (user brief §44, §45) ---
    std::vector<ValidationIssue> Validate() const;
    std::vector<GraphInspectorEntry> Inspect() const;
    size_t NodeCount() const;
    size_t EdgeCount() const;

    // --- Persistence (full-graph durability) ---
    // Serialize captures EVERYTHING user-visible: all nodes (kind, signal,
    // role, name, enabled, states, volume, layout, processing, cost, meta),
    // all routing edges, all output configs + assignments, all bus scenes
    // and named production snapshots. Restore() rebuilds exactly that state
    // on an empty graph. Auto-save: InstallSaver registers a sink invoked
    // after every successful mutation (except during BeginEdit windows —
    // commit saves once); ProductionEngine wires it to the kernel's
    // DatabaseManager so the graph survives restarts.
    Result<json::Value> Serialize() const;
    Result<void> Restore(const json::Value& doc);
    // Flush a pending auto-save immediately (called at engine shutdown).
    Result<void> PersistDirty();
    void InstallSaver(std::function<void(json::Value)> saver);

    // --- Atomic live changes (user brief §23, §24) ---
    // BeginEdit snapshots state; CommitEdit validates the current graph and
    // keeps it (errors roll back); RollbackEdit restores the snapshot.
    // Rollback restores nodes, edges, reverse edges, bus scenes and snapshots,
    // so a failed commit never leaves the graph half-mutated.
    Result<void> BeginEdit();
    Result<void> CommitEdit();
    void RollbackEdit();
    bool InEdit() const { return inEdit_; }

    // Internal state access for the planner/engine.
    const std::map<std::string, NodeInfo, std::less<>>& Nodes() const { return nodes_; }
    const std::map<std::string, std::set<std::string>, std::less<>>& Edges() const {
        return edges_;
    }

private:
    static std::string NextId(const std::map<std::string, NodeInfo, std::less<>>& nodes,
                              std::string_view kind, std::string_view requested);
    void QueuePersist();   // run the installed saver (skipped during edits)
    Result<void> EnsureNode(std::string_view id) const;
    bool Reachable(std::string_view from, std::string_view to,
                   const std::map<std::string, std::set<std::string>, std::less<>>& edges) const;
    void ApplySnapshot(const ProductionSnapshot& snap);
    std::vector<ValidationIssue> ValidateGraph(
        const std::map<std::string, NodeInfo, std::less<>>& nodes,
        const std::map<std::string, std::set<std::string>, std::less<>>& edges,
        const std::map<std::string, OutputConfig, std::less<>>& outputs) const;

    std::map<std::string, NodeInfo, std::less<>> nodes_;
    std::map<std::string, std::set<std::string>, std::less<>> edges_;   // from -> {to}
    std::map<std::string, std::set<std::string>, std::less<>> reverse_; // to -> {from}
    std::map<std::string, OutputConfig, std::less<>> outputs_;
    std::map<std::string, std::map<std::string, BusSnapshot, std::less<>>, std::less<>>
        busScenes_;                                                   // busId -> scene -> snapshot
    std::map<std::string, ProductionSnapshot, std::less<>> snapshots_;

    std::map<std::string, NodeInfo, std::less<>> baseNodes_;          // edit base
    std::map<std::string, OutputConfig, std::less<>> baseOutputs_;
    std::map<std::string, std::set<std::string>, std::less<>> baseEdges_;
    std::map<std::string, std::map<std::string, BusSnapshot, std::less<>>, std::less<>>
        baseBusScenes_;
    std::map<std::string, ProductionSnapshot, std::less<>> baseSnapshots_;
    bool inEdit_ = false;
    uint64_t nodeSeq_ = 0;

    std::function<void(json::Value)> saver_;   // auto-save sink (engine wires the DB)
};

} // namespace bps::production
