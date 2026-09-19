#include "modules/production/ProductionGraph.hpp"

#include <algorithm>
#include <format>

namespace bps::production {

namespace {

bool IsUniversalSink(NodeKind kind) {
    return kind == NodeKind::Data || kind == NodeKind::Control;
}

bool Accepts(SignalType from, const NodeInfo& node) {
    if (IsUniversalSink(node.kind)) return true;
    return node.signalType == from;
}

} // namespace

std::string ProductionGraph::NextId(
    const std::map<std::string, NodeInfo, std::less<>>& nodes,
    std::string_view kind, std::string_view requested) {
    if (!requested.empty()) return std::string(requested);
    std::string base(kind);
    size_t n = 1;
    std::string candidate;
    do {
        candidate = std::format("{}{}", base, n++);
    } while (nodes.count(candidate));
    return candidate;
}

Result<std::string> ProductionGraph::AddSource(std::string_view id,
                                               std::string_view name,
                                               SignalType type, ResourceCost cost) {
    std::string nid = NextId(nodes_, "src", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Source;
    n.signalType = type;
    n.displayName = std::string(name.empty() ? nid : name);
    n.cost = cost;
    n.sourceState = SourceState::Connected;
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddBus(std::string_view id, std::string_view name,
                                            BusRole role, SignalType type) {
    std::string nid = NextId(nodes_, "bus", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Bus;
    n.signalType = type;
    n.role = role;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddProcessor(std::string_view id,
                                                  std::string_view name,
                                                  SignalType type, ResourceCost cost) {
    std::string nid = NextId(nodes_, "proc", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Processor;
    n.signalType = type;
    n.displayName = std::string(name.empty() ? nid : name);
    n.cost = cost;
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddMixer(std::string_view id, std::string_view name,
                                              SignalType type) {
    std::string nid = NextId(nodes_, "mix", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Mixer;
    n.signalType = type;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddScene(std::string_view id, std::string_view name,
                                              SignalType type) {
    std::string nid = NextId(nodes_, "scene", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Scene;
    n.signalType = type;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddOutput(std::string_view id, std::string_view name,
                                               SignalType type, const OutputConfig& cfg) {
    std::string nid = NextId(nodes_, "out", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Output;
    n.signalType = type;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    outputs_[nid] = cfg;
    return nid;
}

Result<std::string> ProductionGraph::AddVirtualSource(std::string_view id,
                                                      std::string_view name,
                                                      SignalType type) {
    std::string nid = NextId(nodes_, "vs", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::VirtualSource;
    n.signalType = type;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddDataNode(std::string_view id, std::string_view name) {
    std::string nid = NextId(nodes_, "data", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Data;
    n.signalType = SignalType::Data;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    return nid;
}

Result<std::string> ProductionGraph::AddControlNode(std::string_view id,
                                                    std::string_view name) {
    std::string nid = NextId(nodes_, "ctrl", id);
    if (nodes_.count(nid)) return Error::Make(Err::Production_AlreadyExists,
                                              "ProductionGraph", "node exists: " + nid);
    NodeInfo n;
    n.id = nid;
    n.kind = NodeKind::Control;
    n.signalType = SignalType::Control;
    n.displayName = std::string(name.empty() ? nid : name);
    nodes_[nid] = std::move(n);
    return nid;
}

Result<void> ProductionGraph::RemoveNode(std::string_view id) {
    auto it = nodes_.find(std::string(id));
    if (it == nodes_.end()) return Error::Make(Err::Production_NodeNotFound,
                                               "ProductionGraph", "no node: " + std::string(id));
    // Drop all edges touching this node.
    for (auto& [from, tos] : edges_) tos.erase(std::string(id));
    edges_.erase(std::string(id));
    for (auto& [to, froms] : reverse_) froms.erase(std::string(id));
    reverse_.erase(std::string(id));
    outputs_.erase(std::string(id));
    nodes_.erase(it);
    return Ok();
}

Result<void> ProductionGraph::EnsureNode(std::string_view id) const {
    if (!nodes_.count(std::string(id)))
        return Error::Make(Err::Production_NodeNotFound, "ProductionGraph",
                           "no node: " + std::string(id));
    return Ok();
}

bool ProductionGraph::Reachable(
    std::string_view from, std::string_view to,
    const std::map<std::string, std::set<std::string>, std::less<>>& edges) const {
    // DFS from `from`; can we reach `to`?
    std::vector<std::string> stack{std::string(from)};
    std::set<std::string> seen;
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (cur == to) return true;
        if (!seen.insert(cur).second) continue;
        auto it = edges.find(cur);
        if (it == edges.end()) continue;
        for (const auto& nxt : it->second) stack.push_back(nxt);
    }
    return false;
}

bool ProductionGraph::WouldCreateCycle(std::string_view from, std::string_view to) const {
    // Adding edge from -> to creates a cycle iff `to` can already reach `from`.
    return Reachable(to, from, edges_);
}

Result<void> ProductionGraph::Connect(std::string_view from, std::string_view to,
                                      SignalType type) {
    if (auto r = EnsureNode(from); !r.ok()) return r;
    if (auto r = EnsureNode(to); !r.ok()) return r;
    if (edges_[std::string(from)].count(std::string(to))) return Ok();  // idempotent
    if (WouldCreateCycle(from, to))
        return Error::Make(Err::Production_CircularRoute, "ProductionGraph",
                           "edge " + std::string(from) + " -> " + std::string(to) +
                               " would create a cycle");
    if (!Accepts(type, nodes_.at(std::string(from))) || !Accepts(type, nodes_.at(std::string(to))))
        return Error::Make(Err::Production_SignalMismatch, "ProductionGraph",
                           "signal type mismatch on " + std::string(from) + " -> " +
                               std::string(to));
    edges_[std::string(from)].insert(std::string(to));
    reverse_[std::string(to)].insert(std::string(from));
    return Ok();
}

Result<void> ProductionGraph::Disconnect(std::string_view from, std::string_view to) {
    auto fit = edges_.find(std::string(from));
    if (fit == edges_.end() || !fit->second.count(std::string(to)))
        return Error::Make(Err::Production_NodeNotFound, "ProductionGraph",
                           "no edge " + std::string(from) + " -> " + std::string(to));
    fit->second.erase(std::string(to));
    reverse_[std::string(to)].erase(std::string(from));
    return Ok();
}

std::vector<std::string> ProductionGraph::TopologicalOrder() const {
    // Kahn: repeatedly emit nodes with no remaining incoming edges.
    std::map<std::string, size_t, std::less<>> indegree;
    for (const auto& [id, n] : nodes_) indegree[id] = 0;
    for (const auto& [from, tos] : edges_)
        for (const auto& to : tos)
            if (indegree.count(to)) ++indegree[to];

    std::vector<std::string> result;
    std::vector<std::string> ready;
    for (const auto& [id, d] : indegree)
        if (d == 0) ready.push_back(id);
    std::sort(ready.begin(), ready.end());

    while (!ready.empty()) {
        std::string cur = ready.back();
        ready.pop_back();
        result.push_back(cur);
        auto it = edges_.find(cur);
        if (it == edges_.end()) continue;
        for (const auto& to : it->second) {
            if (--indegree[to] == 0) ready.push_back(to);
        }
        std::sort(ready.begin(), ready.end());
    }
    // Any remaining nodes belong to a cycle (should not happen — Connect guards).
    for (const auto& [id, d] : indegree)
        if (d > 0) result.push_back(id);
    return result;
}

std::vector<std::string> ProductionGraph::Downstream(std::string_view id) const {
    auto it = edges_.find(std::string(id));
    if (it == edges_.end()) return {};
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

std::vector<std::string> ProductionGraph::Upstream(std::string_view id) const {
    auto it = reverse_.find(std::string(id));
    if (it == reverse_.end()) return {};
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

Result<void> ProductionGraph::SetEnabled(std::string_view id, bool enabled) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].enabled = enabled;
    return Ok();
}

Result<void> ProductionGraph::SetSourceState(std::string_view id, SourceState state) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].sourceState = state;
    return Ok();
}

Result<void> ProductionGraph::SetSceneState(std::string_view id, SceneState state) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].sceneState = state;
    return Ok();
}

Result<void> ProductionGraph::SetVolume(std::string_view id, const VolumeControl& vol) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].volume = vol;
    return Ok();
}

Result<VolumeControl> ProductionGraph::Volume(std::string_view id) const {
    auto it = nodes_.find(std::string(id));
    if (it == nodes_.end()) return Error::Make(Err::Production_NodeNotFound,
                                               "ProductionGraph", "no node: " + std::string(id));
    return it->second.volume;
}

Result<void> ProductionGraph::SetLayout(std::string_view id, ChannelLayout layout) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].layout = layout;
    return Ok();
}

Result<void> ProductionGraph::AddProcessing(std::string_view id, const ProcessingStage& stage) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].processing.push_back(stage);
    return Ok();
}

Result<void> ProductionGraph::ClearProcessing(std::string_view id) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].processing.clear();
    return Ok();
}

Result<void> ProductionGraph::SetFallback(std::string_view sourceId,
                                          std::string_view fallbackId) {
    if (auto r = EnsureNode(sourceId); !r.ok()) return r;
    if (auto r = EnsureNode(fallbackId); !r.ok()) return r;
    if (nodes_.at(std::string(sourceId)).kind != NodeKind::Source)
        return Error::Make(Err::Production_InvalidState, "ProductionGraph",
                           "fallback source is not a Source node");
    if (nodes_.at(std::string(fallbackId)).kind != NodeKind::Source)
        return Error::Make(Err::Production_InvalidState, "ProductionGraph",
                           "fallback target is not a Source node");
    nodes_[std::string(sourceId)].fallbackId = std::string(fallbackId);
    return Ok();
}

Result<NodeInfo> ProductionGraph::GetNode(std::string_view id) const {
    auto it = nodes_.find(std::string(id));
    if (it == nodes_.end()) return Error::Make(Err::Production_NodeNotFound,
                                               "ProductionGraph", "no node: " + std::string(id));
    return it->second;
}

bool ProductionGraph::HasNode(std::string_view id) const {
    return nodes_.count(std::string(id)) != 0;
}

std::vector<std::string> ProductionGraph::NodeIds() const {
    std::vector<std::string> out;
    out.reserve(nodes_.size());
    for (const auto& [id, n] : nodes_) out.push_back(id);
    return out;
}

std::vector<std::string> ProductionGraph::BusIds() const {
    std::vector<std::string> out;
    for (const auto& [id, n] : nodes_)
        if (n.kind == NodeKind::Bus) out.push_back(id);
    return out;
}

std::vector<std::string> ProductionGraph::SourceIds() const {
    std::vector<std::string> out;
    for (const auto& [id, n] : nodes_)
        if (n.kind == NodeKind::Source) out.push_back(id);
    return out;
}

std::vector<std::string> ProductionGraph::OutputIds() const {
    std::vector<std::string> out;
    for (const auto& [id, n] : nodes_)
        if (n.kind == NodeKind::Output) out.push_back(id);
    return out;
}

std::vector<std::string> ProductionGraph::SceneIds() const {
    std::vector<std::string> out;
    for (const auto& [id, n] : nodes_)
        if (n.kind == NodeKind::Scene) out.push_back(id);
    return out;
}

std::vector<std::string> ProductionGraph::VirtualSourceIds() const {
    std::vector<std::string> out;
    for (const auto& [id, n] : nodes_)
        if (n.kind == NodeKind::VirtualSource) out.push_back(id);
    return out;
}

// --- Outputs ---

Result<void> ProductionGraph::AssignOutputBuses(std::string_view outputId,
                                                std::string_view videoBusId,
                                                std::string_view audioBusId) {
    auto oit = outputs_.find(std::string(outputId));
    if (oit == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    if (!videoBusId.empty()) {
        auto it = nodes_.find(std::string(videoBusId));
        if (it == nodes_.end() || it->second.kind != NodeKind::Bus ||
            it->second.signalType != SignalType::Video)
            return Error::Make(Err::Production_BusNotFound, "ProductionGraph",
                               "no video bus: " + std::string(videoBusId));
        oit->second.videoBusId = std::string(videoBusId);
    }
    if (!audioBusId.empty()) {
        auto it = nodes_.find(std::string(audioBusId));
        if (it == nodes_.end() || it->second.kind != NodeKind::Bus ||
            it->second.signalType != SignalType::Audio)
            return Error::Make(Err::Production_BusNotFound, "ProductionGraph",
                               "no audio bus: " + std::string(audioBusId));
        oit->second.audioBusId = std::string(audioBusId);
    }
    return Ok();
}

Result<OutputConfig> ProductionGraph::GetOutputConfig(std::string_view outputId) const {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    return it->second;
}

Result<void> ProductionGraph::SetOutputPriority(std::string_view outputId,
                                                OutputPriority p) {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    it->second.priority = p;
    return Ok();
}

Result<void> ProductionGraph::SetOutputGroup(std::string_view outputId,
                                             std::string_view group) {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    it->second.group = std::string(group);
    return Ok();
}

Result<void> ProductionGraph::SetOutputFailover(std::string_view outputId,
                                                const std::vector<std::string>& order) {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    for (const auto& o : order)
        if (!outputs_.count(o))
            return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                               "failover target is not an output: " + o);
    it->second.failoverOrder = order;
    return Ok();
}

Result<void> ProductionGraph::SetOutputEncoder(std::string_view outputId,
                                               std::string_view encoder) {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    it->second.encoder = std::string(encoder);
    return Ok();
}

// --- Bus scenes ---

Result<void> ProductionGraph::SaveBusScene(std::string_view busId,
                                           std::string_view sceneName) {
    auto it = nodes_.find(std::string(busId));
    if (it == nodes_.end() || it->second.kind != NodeKind::Bus)
        return Error::Make(Err::Production_BusNotFound, "ProductionGraph",
                           "no bus: " + std::string(busId));
    BusSnapshot snap;
    snap.id = std::string(busId);
    snap.name = std::string(sceneName);
    snap.role = it->second.role;
    snap.volume = it->second.volume;
    snap.processing = it->second.processing;
    // Direct inputs currently enabled feed this bus.
    for (const auto& from : reverse_[std::string(busId)])
        if (nodes_.at(from).enabled) snap.enabledInputs.push_back(from);
    for (const auto& [oid, ocfg] : outputs_)
        if (ocfg.videoBusId == busId || ocfg.audioBusId == busId)
            snap.outputs.push_back(oid);
    busScenes_[std::string(busId)][std::string(sceneName)] = std::move(snap);
    return Ok();
}

Result<void> ProductionGraph::ApplyBusScene(std::string_view busId,
                                            std::string_view sceneName) {
    auto bit = busScenes_.find(std::string(busId));
    if (bit == busScenes_.end()) return Error::Make(Err::Production_BusNotFound,
                                                    "ProductionGraph", "no bus: " +
                                                        std::string(busId));
    auto sit = bit->second.find(std::string(sceneName));
    if (sit == bit->second.end())
        return Error::Make(Err::Production_SnapshotNotFound, "ProductionGraph",
                           "no bus scene '" + std::string(sceneName) + "' on " +
                               std::string(busId));
    const auto& snap = sit->second;
    auto it = nodes_.find(std::string(busId));
    if (it == nodes_.end()) return Error::Make(Err::Production_BusNotFound,
                                               "ProductionGraph", "no bus: " +
                                                   std::string(busId));
    it->second.volume = snap.volume;
    it->second.processing = snap.processing;
    // Re-activate the saved input set; disable direct inputs not in it.
    for (const auto& from : reverse_[std::string(busId)]) {
        bool want = std::find(snap.enabledInputs.begin(), snap.enabledInputs.end(), from) !=
                    snap.enabledInputs.end();
        nodes_[from].enabled = want;
    }
    return Ok();
}

std::vector<std::string> ProductionGraph::BusSceneNames(std::string_view busId) const {
    std::vector<std::string> out;
    auto it = busScenes_.find(std::string(busId));
    if (it == busScenes_.end()) return out;
    for (const auto& [name, s] : it->second) out.push_back(name);
    return out;
}

// --- Production snapshots ---

Result<std::string> ProductionGraph::SaveSnapshot(std::string_view name) {
    ProductionSnapshot snap;
    snap.timestampMs = 0;
    snap.name = std::string(name);
    snap.metadata["nodes"] = std::to_string(nodes_.size());
    for (const auto& [bid, n] : nodes_) {
        if (n.kind != NodeKind::Bus) continue;
        BusSnapshot bs;
        bs.id = bid;
        bs.role = n.role;
        bs.volume = n.volume;
        bs.processing = n.processing;
        for (const auto& from : reverse_[bid])
            if (nodes_.at(from).enabled) bs.enabledInputs.push_back(from);
        for (const auto& [oid, ocfg] : outputs_)
            if (ocfg.videoBusId == bid || ocfg.audioBusId == bid) bs.outputs.push_back(oid);
        snap.buses.push_back(std::move(bs));
    }
    for (const auto& [oid, ocfg] : outputs_) {
        snap.outputAssignments.emplace_back(oid, ocfg.audioBusId);
        snap.outputAssignments.emplace_back(oid + ":video", ocfg.videoBusId);
    }
    snapshots_[std::string(name)] = std::move(snap);
    return std::string(name);
}

void ProductionGraph::ApplySnapshot(const ProductionSnapshot& snap) {
    // Restore bus states.
    for (const auto& bs : snap.buses) {
        auto it = nodes_.find(bs.id);
        if (it == nodes_.end() || it->second.kind != NodeKind::Bus) continue;
        it->second.volume = bs.volume;
        it->second.processing = bs.processing;
        for (const auto& from : reverse_[bs.id]) {
            bool want = std::find(bs.enabledInputs.begin(), bs.enabledInputs.end(), from) !=
                        bs.enabledInputs.end();
            nodes_[from].enabled = want;
        }
    }
    // Restore output bus assignments.
    for (const auto& [key, bus] : snap.outputAssignments) {
        std::string oid = key;
        bool video = false;
        auto suffix = oid.rfind(":video");
        if (suffix != std::string::npos && suffix + 6 == oid.size()) {
            oid = oid.substr(0, suffix);
            video = true;
        }
        auto it = outputs_.find(oid);
        if (it == outputs_.end()) continue;
        if (video) it->second.videoBusId = bus;
        else it->second.audioBusId = bus;
    }
}

Result<void> ProductionGraph::RestoreSnapshot(std::string_view name) {
    auto it = snapshots_.find(std::string(name));
    if (it == snapshots_.end())
        return Error::Make(Err::Production_SnapshotNotFound, "ProductionGraph",
                           "no snapshot: " + std::string(name));
    ApplySnapshot(it->second);
    return Ok();
}

std::vector<std::string> ProductionGraph::SnapshotNames() const {
    std::vector<std::string> out;
    for (const auto& [name, s] : snapshots_) out.push_back(name);
    return out;
}

Result<ProductionSnapshot> ProductionGraph::GetSnapshot(std::string_view name) const {
    auto it = snapshots_.find(std::string(name));
    if (it == snapshots_.end())
        return Error::Make(Err::Production_SnapshotNotFound, "ProductionGraph",
                           "no snapshot: " + std::string(name));
    return it->second;
}

// --- Validation + inspection ---

std::vector<ValidationIssue> ProductionGraph::ValidateGraph(
    const std::map<std::string, NodeInfo, std::less<>>& nodes,
    const std::map<std::string, std::set<std::string>, std::less<>>& edges,
    const std::map<std::string, OutputConfig, std::less<>>& outputs) const {
    std::vector<ValidationIssue> issues;

    for (const auto& [from, tos] : edges) {
        if (!nodes.count(from)) {
            issues.push_back({ValidationIssue::Severity::Error, "missing_node",
                              "edge source missing: " + from});
            continue;
        }
        for (const auto& to : tos) {
            if (!nodes.count(to)) {
                issues.push_back({ValidationIssue::Severity::Error, "missing_node",
                                  "edge target missing: " + to});
                continue;
            }
            const auto& fn = nodes.at(from);
            const auto& tn = nodes.at(to);
            if (!Accepts(fn.signalType, tn) || !Accepts(fn.signalType, fn)) {
                issues.push_back({ValidationIssue::Severity::Error, "signal_mismatch",
                                  from + " -> " + to + " mixes incompatible signal types"});
            }
        }
    }

    for (const auto& [oid, cfg] : outputs) {
        if (cfg.videoBusId.empty() && cfg.audioBusId.empty()) {
            issues.push_back({ValidationIssue::Severity::Warning, "output_no_bus",
                              "output " + oid + " has no video or audio bus"});
            continue;
        }
        if (!cfg.videoBusId.empty()) {
            auto it = nodes.find(cfg.videoBusId);
            if (it == nodes.end() || it->second.kind != NodeKind::Bus)
                issues.push_back({ValidationIssue::Severity::Error, "output_no_bus",
                                  "output " + oid + " video bus missing: " + cfg.videoBusId});
        }
        if (!cfg.audioBusId.empty()) {
            auto it = nodes.find(cfg.audioBusId);
            if (it == nodes.end() || it->second.kind != NodeKind::Bus)
                issues.push_back({ValidationIssue::Severity::Error, "output_no_bus",
                                  "output " + oid + " audio bus missing: " + cfg.audioBusId});
        }
    }

    // Build the reverse adjacency from the passed edges so validation is
    // self-contained (independent of the live reverse_ member).
    std::map<std::string, std::set<std::string>, std::less<>> reverse;
    for (const auto& [from, tos] : edges)
        for (const auto& to : tos)
            if (nodes.count(to)) reverse[to].insert(from);

    for (const auto& [id, n] : nodes) {
        if (n.kind == NodeKind::Bus) {
            auto rit = reverse.find(id);
            bool hasInput = rit != reverse.end() && !rit->second.empty();
            if (!hasInput)
                issues.push_back({ValidationIssue::Severity::Warning, "bus_empty",
                                  "bus " + id + " has no inputs"});
        }
        if (n.kind == NodeKind::Source) {
            auto eit = edges.find(id);
            bool hasOutput = eit != edges.end() && !eit->second.empty();
            if (!hasOutput)
                issues.push_back({ValidationIssue::Severity::Warning, "source_unrouted",
                                  "source " + id + " feeds nothing"});
        }
    }
    return issues;
}

std::vector<ValidationIssue> ProductionGraph::Validate() const {
    return ValidateGraph(nodes_, edges_, outputs_);
}

std::vector<GraphInspectorEntry> ProductionGraph::Inspect() const {
    std::vector<GraphInspectorEntry> out;
    auto order = TopologicalOrder();
    for (const auto& id : order) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) continue;
        GraphInspectorEntry e;
        e.nodeId = id;
        e.kind = it->second.kind;
        e.cost = it->second.cost;
        e.active = it->second.active;
        auto up = Upstream(id);
        e.upstream = up.empty() ? "" : up.front();
        e.feeds = Downstream(id);
        out.push_back(std::move(e));
    }
    return out;
}

size_t ProductionGraph::NodeCount() const { return nodes_.size(); }
size_t ProductionGraph::EdgeCount() const {
    size_t n = 0;
    for (const auto& [from, tos] : edges_) n += tos.size();
    return n;
}

// --- Atomic live changes ---

Result<void> ProductionGraph::BeginEdit() {
    if (inEdit_) return Error::Make(Err::Production_InvalidState, "ProductionGraph",
                                    "edit already in progress");
    baseNodes_ = nodes_;
    baseOutputs_ = outputs_;
    baseEdges_ = edges_;
    baseBusScenes_ = busScenes_;
    baseSnapshots_ = snapshots_;
    inEdit_ = true;
    return Ok();
}

Result<void> ProductionGraph::CommitEdit() {
    if (!inEdit_) return Error::Make(Err::Production_InvalidState, "ProductionGraph",
                                     "no edit in progress");
    auto issues = Validate();
    if (!issues.empty() && issues.front().severity == ValidationIssue::Severity::Error) {
        RollbackEdit();
        return Error::Make(Err::Production_ValidationFailed, "ProductionGraph",
                           "commit rejected: " + issues.front().message);
    }
    inEdit_ = false;
    baseNodes_.clear();
    baseOutputs_.clear();
    baseEdges_.clear();
    baseBusScenes_.clear();
    baseSnapshots_.clear();
    return Ok();
}

void ProductionGraph::RollbackEdit() {
    if (!inEdit_) return;
    nodes_ = baseNodes_;
    outputs_ = baseOutputs_;
    edges_ = baseEdges_;
    // Rebuild the reverse adjacency from the restored edges.
    reverse_.clear();
    for (const auto& [from, tos] : edges_)
        for (const auto& to : tos) reverse_[to].insert(from);
    busScenes_ = baseBusScenes_;
    snapshots_ = baseSnapshots_;
    inEdit_ = false;
    baseNodes_.clear();
    baseOutputs_.clear();
    baseEdges_.clear();
    baseBusScenes_.clear();
    baseSnapshots_.clear();
}

} // namespace bps::production
