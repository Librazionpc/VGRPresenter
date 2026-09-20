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

// --- Persistence helpers (enum <-> stable string tokens) ---

constexpr const char* kSignalNames[] = {"audio", "video", "data", "control"};
constexpr const char* kKindNames[] = {"source", "processor", "bus", "mixer",
                                      "scene", "output", "virtual_source",
                                      "data", "control"};
constexpr const char* kRoleNames[] = {"source", "group", "program", "monitor",
                                      "stream", "recording", "custom"};
constexpr const char* kSourceStateNames[] = {"connected", "stable", "degraded",
                                             "disconnected", "failed"};
constexpr const char* kSceneStateNames[] = {"preview", "program", "standby",
                                            "disabled", "emergency"};
constexpr const char* kLayoutNames[] = {"mono", "stereo", "surround21",
                                        "surround51", "surround71", "multi"};
constexpr const char* kProcessKindNames[] = {"gate", "eq", "compressor",
                                             "limiter", "color_correct", "delay",
                                             "gain", "denoise", "custom"};
constexpr const char* kPriorityNames[] = {"critical", "high", "medium", "low"};

template <typename Enum, size_t N>
std::string EnumName(Enum e, const char* const (&names)[N]) {
    auto i = static_cast<size_t>(e);
    return i < N ? names[i] : std::string(names[N - 1]);
}

template <typename Enum, size_t N>
Enum EnumValue(std::string_view name, const char* const (&names)[N], Enum dflt) {
    for (size_t i = 0; i < N; ++i)
        if (name == names[i]) return static_cast<Enum>(i);
    return dflt;
}

json::Value JStr(std::string s) { return json::Value::String(std::move(s)); }
json::Value JNum(double d) { return json::Value::Number(d); }
json::Value JBool(bool b) { return json::Value::Bool(b); }

json::Value VolumeToJson(const VolumeControl& v) {
    json::Value::Object o;
    o["gainDb"] = JNum(v.gainDb);
    o["mute"] = JBool(v.mute);
    o["solo"] = JBool(v.solo);
    o["pan"] = JNum(v.pan);
    o["balance"] = JNum(v.balance);
    return json::Value(std::move(o));
}

VolumeControl VolumeFromJson(const json::Value& v) {
    VolumeControl out;
    if (auto* g = v.Find("gainDb")) out.gainDb = g->asNumber();
    if (auto* m = v.Find("mute")) out.mute = m->asBool();
    if (auto* s = v.Find("solo")) out.solo = s->asBool();
    if (auto* p = v.Find("pan")) out.pan = p->asNumber();
    if (auto* b = v.Find("balance")) out.balance = b->asNumber();
    return out;
}

json::Value CostToJson(const ResourceCost& c) {
    json::Value::Object o;
    o["gpu"] = JNum(c.gpu);
    o["cpu"] = JNum(c.cpu);
    o["vramMb"] = JNum(c.vramMb);
    o["ramMb"] = JNum(c.ramMb);
    return json::Value(std::move(o));
}

ResourceCost CostFromJson(const json::Value& v) {
    ResourceCost out;
    if (auto* g = v.Find("gpu")) out.gpu = static_cast<int>(g->asNumber());
    if (auto* c = v.Find("cpu")) out.cpu = static_cast<int>(c->asNumber());
    if (auto* r = v.Find("vramMb")) out.vramMb = static_cast<int>(r->asNumber());
    if (auto* r = v.Find("ramMb")) out.ramMb = static_cast<int>(r->asNumber());
    return out;
}

json::Value StageToJson(const ProcessingStage& s) {
    json::Value::Object o;
    o["kind"] = JStr(EnumName(s.kind, kProcessKindNames));
    o["amount"] = JNum(s.amount);
    o["custom"] = JStr(s.custom);
    return json::Value(std::move(o));
}

ProcessingStage StageFromJson(const json::Value& v) {
    ProcessingStage out;
    if (auto* k = v.Find("kind"))
        out.kind = EnumValue(k->asString("gain"), kProcessKindNames, ProcessKind::Gain);
    if (auto* a = v.Find("amount")) out.amount = a->asNumber(1.0);
    if (auto* c = v.Find("custom")) out.custom = std::string(c->asString());
    return out;
}

// The serialized doc also records the "next bus" counter the app model uses
// for id generation, so ids stay stable across restarts.
constexpr const char* kDocKey = "v1";

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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::Disconnect(std::string_view from, std::string_view to) {
    auto fit = edges_.find(std::string(from));
    if (fit == edges_.end() || !fit->second.count(std::string(to)))
        return Error::Make(Err::Production_NodeNotFound, "ProductionGraph",
                           "no edge " + std::string(from) + " -> " + std::string(to));
    fit->second.erase(std::string(to));
    reverse_[std::string(to)].erase(std::string(from));
    if (fit->second.empty()) edges_.erase(fit);   // no dangling empty sets
    QueuePersist();
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
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::RenameNode(std::string_view id, std::string_view name) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].displayName = std::string(name);
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::SetSourceState(std::string_view id, SourceState state) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].sourceState = state;
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::SetSceneState(std::string_view id, SceneState state) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].sceneState = state;
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::SetVolume(std::string_view id, const VolumeControl& vol) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].volume = vol;
    QueuePersist();
    return Ok();
}

Result<VolumeControl> ProductionGraph::Volume(std::string_view id) const {
    auto it = nodes_.find(std::string(id));
    if (it == nodes_.end()) return Error::Make(Err::Production_NodeNotFound,
                                               "ProductionGraph", "no node: " + std::string(id));
    return it->second.volume;
}

Result<void> ProductionGraph::SetMeta(std::string_view id, std::string_view key,
                                      std::string_view value) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].meta[std::string(key)] = std::string(value);
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::SetLayout(std::string_view id, ChannelLayout layout) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].layout = layout;
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::AddProcessing(std::string_view id, const ProcessingStage& stage) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].processing.push_back(stage);
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::ClearProcessing(std::string_view id) {
    if (auto r = EnsureNode(id); !r.ok()) return r;
    nodes_[std::string(id)].processing.clear();
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::SetOutputGroup(std::string_view outputId,
                                             std::string_view group) {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    it->second.group = std::string(group);
    QueuePersist();
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
    QueuePersist();
    return Ok();
}

Result<void> ProductionGraph::SetOutputEncoder(std::string_view outputId,
                                               std::string_view encoder) {
    auto it = outputs_.find(std::string(outputId));
    if (it == outputs_.end())
        return Error::Make(Err::Production_OutputNotFound, "ProductionGraph",
                           "no output: " + std::string(outputId));
    it->second.encoder = std::string(encoder);
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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
    QueuePersist();
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

// --- Persistence (full-graph durability) ---

Result<json::Value> ProductionGraph::Serialize() const {
    json::Value::Object root;
    root["version"] = JNum(1);

    json::Value::Array nodeArr;
    nodeArr.reserve(nodes_.size());
    for (const auto& [id, n] : nodes_) {
        json::Value::Object o;
        o["id"] = JStr(n.id);
        o["kind"] = JStr(EnumName(n.kind, kKindNames));
        o["signal"] = JStr(EnumName(n.signalType, kSignalNames));
        o["role"] = JStr(EnumName(n.role, kRoleNames));
        o["name"] = JStr(n.displayName);
        o["enabled"] = JBool(n.enabled);
        o["sourceState"] = JStr(EnumName(n.sourceState, kSourceStateNames));
        o["sceneState"] = JStr(EnumName(n.sceneState, kSceneStateNames));
        o["layout"] = JStr(EnumName(n.layout, kLayoutNames));
        o["volume"] = VolumeToJson(n.volume);
        if (!n.processing.empty()) {
            json::Value::Array stages;
            for (const auto& s : n.processing) stages.push_back(StageToJson(s));
            o["processing"] = json::Value(std::move(stages));
        }
        o["cost"] = CostToJson(n.cost);
        if (!n.fallbackId.empty()) o["fallback"] = JStr(n.fallbackId);
        if (!n.meta.empty()) {
            json::Value::Object m;
            for (const auto& [k, v] : n.meta) m[k] = JStr(v);
            o["meta"] = json::Value(std::move(m));
        }
        nodeArr.push_back(json::Value(std::move(o)));
    }
    root["nodes"] = json::Value(std::move(nodeArr));

    json::Value::Array edgeArr;
    for (const auto& [from, tos] : edges_)
        for (const auto& to : tos) {
            json::Value::Object e;
            e["from"] = JStr(from);
            e["to"] = JStr(to);
            edgeArr.push_back(json::Value(std::move(e)));
        }
    root["edges"] = json::Value(std::move(edgeArr));

    json::Value::Object outputs;
    for (const auto& [oid, cfg] : outputs_) {
        json::Value::Object o;
        o["videoBus"] = JStr(cfg.videoBusId);
        o["audioBus"] = JStr(cfg.audioBusId);
        o["priority"] = JStr(EnumName(cfg.priority, kPriorityNames));
        o["group"] = JStr(cfg.group);
        o["encoder"] = JStr(cfg.encoder);
        o["networkTarget"] = JStr(cfg.networkTarget);
        if (!cfg.failoverOrder.empty()) {
            json::Value::Array f;
            for (const auto& fid : cfg.failoverOrder) f.push_back(JStr(fid));
            o["failover"] = json::Value(std::move(f));
        }
        outputs[oid] = json::Value(std::move(o));
    }
    root["outputs"] = json::Value(std::move(outputs));

    json::Value::Object scenes;
    for (const auto& [busId, busScenes] : busScenes_) {
        json::Value::Object s;
        for (const auto& [name, snap] : busScenes) {
            json::Value::Object sc;
            sc["role"] = JStr(EnumName(snap.role, kRoleNames));
            sc["volume"] = VolumeToJson(snap.volume);
            json::Value::Array ins;
            for (const auto& in : snap.enabledInputs) ins.push_back(JStr(in));
            sc["inputs"] = json::Value(std::move(ins));
            json::Value::Array outs;
            for (const auto& oid : snap.outputs) outs.push_back(JStr(oid));
            sc["outputs"] = json::Value(std::move(outs));
            json::Value::Array st;
            for (const auto& stage : snap.processing) st.push_back(StageToJson(stage));
            sc["processing"] = json::Value(std::move(st));
            s[name] = json::Value(std::move(sc));
        }
        scenes[busId] = json::Value(std::move(s));
    }
    root["busScenes"] = json::Value(std::move(scenes));

    json::Value::Object snaps;
    for (const auto& [name, snap] : snapshots_) {
        json::Value::Object sc;
        sc["timestampMs"] = JNum(static_cast<double>(snap.timestampMs));
        json::Value::Object meta;
        for (const auto& [k, v] : snap.metadata) meta[k] = JStr(v);
        sc["metadata"] = json::Value(std::move(meta));
        snaps[name] = json::Value(std::move(sc));
    }
    root["snapshots"] = json::Value(std::move(snaps));

    return json::Value(std::move(root));
}

Result<void> ProductionGraph::Restore(const json::Value& doc) {
    auto* version = doc.Find("version");
    if (!version || static_cast<int>(version->asNumber()) != 1)
        return Error::Make(Err::Config_ParseFailed, "ProductionGraph",
                           "unsupported graph document version");

    auto* nodes = doc.Find("nodes");
    auto* edges = doc.Find("edges");
    if (!nodes || !nodes->asArray() || !edges || !edges->asArray())
        return Error::Make(Err::Config_ParseFailed, "ProductionGraph",
                           "graph document missing nodes/edges");

    // Rebuild into locals first so a malformed doc never half-clobbers the
    // live graph.
    std::map<std::string, NodeInfo, std::less<>> newNodes;
    for (const auto& nv : *nodes->asArray()) {
        auto* idV = nv.Find("id");
        auto* kindV = nv.Find("kind");
        if (!idV || !kindV) continue;
        NodeInfo n;
        n.id = std::string(idV->asString());
        n.kind = EnumValue(kindV->asString("source"), kKindNames, NodeKind::Source);
        if (auto* v = nv.Find("signal"))
            n.signalType = EnumValue(v->asString("audio"), kSignalNames, SignalType::Audio);
        if (auto* v = nv.Find("role"))
            n.role = EnumValue(v->asString("custom"), kRoleNames, BusRole::Custom);
        if (auto* v = nv.Find("name")) n.displayName = std::string(v->asString());
        if (auto* v = nv.Find("enabled")) n.enabled = v->asBool(true);
        if (auto* v = nv.Find("sourceState"))
            n.sourceState = EnumValue(v->asString("connected"), kSourceStateNames,
                                      SourceState::Connected);
        if (auto* v = nv.Find("sceneState"))
            n.sceneState = EnumValue(v->asString("standby"), kSceneStateNames,
                                     SceneState::Standby);
        if (auto* v = nv.Find("layout"))
            n.layout = EnumValue(v->asString("stereo"), kLayoutNames, ChannelLayout::Stereo);
        if (auto* v = nv.Find("volume")) n.volume = VolumeFromJson(*v);
        if (auto* v = nv.Find("processing"))
            if (auto* arr = v->asArray())
                for (const auto& sv : *arr) n.processing.push_back(StageFromJson(sv));
        if (auto* v = nv.Find("cost")) n.cost = CostFromJson(*v);
        if (auto* v = nv.Find("fallback")) n.fallbackId = std::string(v->asString());
        if (auto* v = nv.Find("meta"))
            if (auto* obj = v->asObject())
                for (const auto& [k, mv] : *obj) n.meta[k] = std::string(mv.asString());
        newNodes[n.id] = std::move(n);
    }

    std::map<std::string, std::set<std::string>, std::less<>> newEdges;
    std::map<std::string, std::set<std::string>, std::less<>> newReverse;
    for (const auto& ev : *edges->asArray()) {
        auto* from = ev.Find("from");
        auto* to = ev.Find("to");
        if (!from || !to) continue;
        const std::string f = std::string(from->asString());
        const std::string t = std::string(to->asString());
        // Only edges between restored nodes survive.
        if (!newNodes.count(f) || !newNodes.count(t)) continue;
        newEdges[f].insert(t);
        newReverse[t].insert(f);
    }

    std::map<std::string, OutputConfig, std::less<>> newOutputs;
    if (auto* outs = doc.Find("outputs"))
        if (auto* obj = outs->asObject())
            for (const auto& [oid, ov] : *obj) {
                OutputConfig cfg;
                if (auto* v = ov.Find("videoBus")) cfg.videoBusId = std::string(v->asString());
                if (auto* v = ov.Find("audioBus")) cfg.audioBusId = std::string(v->asString());
                if (auto* v = ov.Find("priority"))
                    cfg.priority = EnumValue(v->asString("medium"), kPriorityNames,
                                             OutputPriority::Medium);
                if (auto* v = ov.Find("group")) cfg.group = std::string(v->asString());
                if (auto* v = ov.Find("encoder")) cfg.encoder = std::string(v->asString());
                if (auto* v = ov.Find("networkTarget"))
                    cfg.networkTarget = std::string(v->asString());
                if (auto* v = ov.Find("failover"))
                    if (auto* arr = v->asArray())
                        for (const auto& fv : *arr)
                            cfg.failoverOrder.push_back(std::string(fv.asString()));
                newOutputs[oid] = std::move(cfg);
            }

    std::map<std::string, std::map<std::string, BusSnapshot, std::less<>>, std::less<>>
        newScenes;
    if (auto* allScenes = doc.Find("busScenes"))
        if (auto* obj = allScenes->asObject())
            for (const auto& [busId, scenesV] : *obj) {
                auto* scenesObj = scenesV.asObject();
                if (!scenesObj) continue;
                for (const auto& [name, sv] : *scenesObj) {
                    BusSnapshot snap;
                    snap.id = busId;
                    snap.name = name;
                    if (auto* v = sv.Find("role"))
                        snap.role = EnumValue(v->asString("group"), kRoleNames, BusRole::Group);
                    if (auto* v = sv.Find("volume")) snap.volume = VolumeFromJson(*v);
                    if (auto* v = sv.Find("inputs"))
                        if (auto* arr = v->asArray())
                            for (const auto& iv : *arr)
                                snap.enabledInputs.push_back(std::string(iv.asString()));
                    if (auto* v = sv.Find("outputs"))
                        if (auto* arr = v->asArray())
                            for (const auto& ov : *arr)
                                snap.outputs.push_back(std::string(ov.asString()));
                    if (auto* v = sv.Find("processing"))
                        if (auto* arr = v->asArray())
                            for (const auto& pv : *arr)
                                snap.processing.push_back(StageFromJson(pv));
                    newScenes[busId][name] = std::move(snap);
                }
            }

    std::map<std::string, ProductionSnapshot, std::less<>> newSnaps;
    if (auto* allSnaps = doc.Find("snapshots"))
        if (auto* obj = allSnaps->asObject())
            for (const auto& [name, sv] : *obj) {
                ProductionSnapshot snap;
                snap.name = name;
                if (auto* v = sv.Find("timestampMs"))
                    snap.timestampMs = static_cast<int64_t>(v->asNumber());
                if (auto* v = sv.Find("metadata"))
                    if (auto* mobj = v->asObject())
                        for (const auto& [k, mv] : *mobj)
                            snap.metadata[k] = std::string(mv.asString());
                newSnaps[name] = std::move(snap);
            }

    // Commit: swap everything in atomically.
    nodes_ = std::move(newNodes);
    edges_ = std::move(newEdges);
    reverse_ = std::move(newReverse);
    outputs_ = std::move(newOutputs);
    busScenes_ = std::move(newScenes);
    snapshots_ = std::move(newSnaps);
    return Ok();
}

void ProductionGraph::InstallSaver(std::function<void(json::Value)> saver) {
    saver_ = std::move(saver);
}

void ProductionGraph::QueuePersist() {
    if (inEdit_) return;   // edit windows save once at commit
    if (auto doc = Serialize(); doc.ok() && saver_) saver_(doc.value());
}

Result<void> ProductionGraph::PersistDirty() {
    if (auto doc = Serialize(); doc.ok() && saver_) saver_(doc.value());
    return Ok();
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
    QueuePersist();   // commit changed state — save it
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
