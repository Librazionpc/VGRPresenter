#include "modules/rendering/Pipeline.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace bps::rendering {

PassId PassForLayer(LayerKind kind) {
    switch (kind) {
        case LayerKind::Background: return PassId::Background;
        case LayerKind::Video:      return PassId::Video;
        case LayerKind::Image:      return PassId::Image;
        case LayerKind::Text:       return PassId::Text;
        case LayerKind::Overlay:    return PassId::Overlay;
        case LayerKind::Debug:      return PassId::Debug;
        case LayerKind::Custom:     return PassId::Overlay;
    }
    return PassId::Image;
}

// ---------------------------------------------------------------------------
// RenderPipeline
// ---------------------------------------------------------------------------

void RenderPipeline::Distribute(const std::vector<RenderObject*>& objects,
                                const std::vector<Layer>& layers) {
    ClearPasses();
    for (auto* obj : objects) {
        if (!obj || !obj->Visible()) continue;
        // Objects render only when their layer exists and is enabled.
        bool layerOk = false;
        LayerKind kind = LayerKind::Image;
        for (const auto& l : layers) {
            if (l.id == obj->LayerId()) {
                layerOk = l.enabled;
                kind = l.kind;
                break;
            }
        }
        if (!layerOk) continue;
        PassId pass = PassForLayer(kind);
        passes_[pass]->AddObject(obj);
    }
}

void RenderPipeline::Execute(const DrawFn& draw) const {
    for (int i = 0; i < static_cast<int>(PassId::Count); ++i) {
        const auto* pass = passes_.at(static_cast<PassId>(i)).get();
        if (!pass || pass->Count() == 0) continue;
        draw(pass->Id(), pass->Objects());
    }
}

size_t RenderPipeline::TotalObjects() const {
    size_t n = 0;
    for (int i = 0; i < static_cast<int>(PassId::Count); ++i)
        n += passes_.at(static_cast<PassId>(i))->Count();
    return n;
}

void RenderPipeline::ClearPasses() {
    for (int i = 0; i < static_cast<int>(PassId::Count); ++i)
        passes_.at(static_cast<PassId>(i))->Clear();
}

// ---------------------------------------------------------------------------
// RenderGraph
// ---------------------------------------------------------------------------

Result<void> RenderGraph::AddNode(const GraphNode& node) {
    for (const auto& n : nodes_)
        if (n.id == node.id)
            return Error::Make(Err::AlreadyExists, "RenderGraph",
                               "node '" + node.id + "' exists");
    nodes_.push_back(node);
    return Ok();
}

Result<void> RenderGraph::RemoveNode(std::string_view id) {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->id == id) {
            nodes_.erase(it);
            passFns_.erase(std::string(id));
            return Ok();
        }
    }
    return Error::Make(Err::Render_InvalidPass, "RenderGraph",
                       "node '" + std::string(id) + "' not found");
}

Result<std::vector<std::string>> RenderGraph::ExecutionOrder() const {
    // Kahn's algorithm (deterministic: sort by priority then id).
    std::map<std::string, const GraphNode*> byId;
    std::map<std::string, int> indegree;
    for (const auto& n : nodes_) {
        byId[n.id] = &n;
        indegree[n.id] = static_cast<int>(n.dependsOn.size());
    }
    std::vector<std::string> order;
    std::vector<const GraphNode*> ready;
    for (const auto& n : nodes_)
        if (indegree[n.id] == 0) ready.push_back(&n);
    std::sort(ready.begin(), ready.end(),
              [](const GraphNode* a, const GraphNode* b) {
                  if (a->priority != b->priority) return a->priority < b->priority;
                  return a->id < b->id;
              });

    while (!ready.empty()) {
        const GraphNode* n = ready.front();
        ready.erase(ready.begin());
        order.push_back(n->id);
        for (const auto& other : nodes_) {
            if (other.id == n->id) continue;
            bool dependsOnN = false;
            for (const auto& dep : other.dependsOn)
                if (dep == n->id) { dependsOnN = true; break; }
            if (dependsOnN) {
                --indegree[other.id];
                if (indegree[other.id] == 0) ready.push_back(&other);
            }
        }
        std::sort(ready.begin(), ready.end(),
                  [](const GraphNode* a, const GraphNode* b) {
                      if (a->priority != b->priority) return a->priority < b->priority;
                      return a->id < b->id;
                  });
    }
    if (order.size() != nodes_.size())
        return Error::Make(Err::Render_InvalidPass, "RenderGraph", "cycle detected");
    return order;
}

Result<void> RenderGraph::RegisterPassFn(std::string_view nodeId, PassFn fn) {
    bool found = false;
    for (const auto& n : nodes_)
        if (n.id == nodeId) { found = true; break; }
    if (!found)
        return Error::Make(Err::Render_InvalidPass, "RenderGraph",
                           "node '" + std::string(nodeId) + "' not found");
    passFns_[std::string(nodeId)] = std::move(fn);
    return Ok();
}

void RenderGraph::ExecutePasses() const {
    auto order = ExecutionOrder();
    if (!order.ok()) return;
    for (const auto& id : order.value()) {
        auto it = passFns_.find(id);
        if (it != passFns_.end()) {
            // Pass functions iterate the objects registered for their node's
            // pass via the pipeline (the renderer wires this per frame).
            it->second(nullptr);
        }
    }
}

} // namespace bps::rendering
