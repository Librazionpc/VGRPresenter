#pragma once

// Render Pipeline + Render Graph (docs/specs/17 §Render Pipeline / §Render
// Graph). The pipeline owns the fixed pass order — nothing renders outside it.
// The RenderGraph is the extensible layer: custom passes register as nodes with
// dependencies and execute in topological order without modifying the renderer.

#include "core/common/Common.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bps::rendering {

// --- Render passes (fixed order; nothing bypasses the pipeline) -------------
enum class PassId : int {
    Background = 0,
    Video,
    Image,
    Text,
    Overlay,
    Effects,
    Debug,
    Count,
};

inline const char* ToString(PassId p) {
    switch (p) {
        case PassId::Background: return "Background";
        case PassId::Video:      return "Video";
        case PassId::Image:      return "Image";
        case PassId::Text:       return "Text";
        case PassId::Overlay:    return "Overlay";
        case PassId::Effects:    return "Effects";
        case PassId::Debug:      return "Debug";
        case PassId::Count:      return "Count";
    }
    return "Unknown";
}

// Map a layer kind onto its render pass.
PassId PassForLayer(LayerKind kind);

// One pass: collects its renderables then renders them in order.
class RenderPass {
public:
    explicit RenderPass(PassId id) : id_(id) {}
    PassId Id() const { return id_; }

    void AddObject(RenderObject* obj) { objects_.push_back(obj); }
    void Clear() { objects_.clear(); }
    const std::vector<RenderObject*>& Objects() const { return objects_; }
    size_t Count() const { return objects_.size(); }

private:
    PassId id_;
    std::vector<RenderObject*> objects_;
};

// RenderPipeline: the ordered passes. Execute() distributes scene objects into
// passes by layer kind, then invokes the draw callback per object in pass order.
class RenderPipeline {
public:
    RenderPipeline() {
        for (int i = 0; i < static_cast<int>(PassId::Count); ++i)
            passes_[static_cast<PassId>(i)] = std::make_unique<RenderPass>(
                static_cast<PassId>(i));
    }

    RenderPass* Pass(PassId id) { return passes_.at(id).get(); }
    const RenderPass* Pass(PassId id) const { return passes_.at(id).get(); }

    // Distribute objects into passes by their layer kind (layers map to passes).
    // Objects without a matching enabled layer are skipped.
    void Distribute(const std::vector<RenderObject*>& objects,
                    const std::vector<Layer>& layers);

    // Draw every pass in order via the callback (passId, pass objects).
    using DrawFn = std::function<void(PassId, const std::vector<RenderObject*>&)>;
    void Execute(const DrawFn& draw) const;

    size_t TotalObjects() const;
    void ClearPasses();

private:
    std::map<PassId, std::unique_ptr<RenderPass>> passes_;
};

// --- Render Graph (extensible passes) ---------------------------------------
struct GraphNode {
    std::string id;
    PassId pass = PassId::Image;      // which fixed pass it extends
    std::vector<std::string> dependsOn;   // node ids that must run first
    int priority = 0;
};

class RenderGraph {
public:
    Result<void> AddNode(const GraphNode& node);
    Result<void> RemoveNode(std::string_view id);
    Result<std::vector<std::string>> ExecutionOrder() const;   // topological sort
    const std::vector<GraphNode>& Nodes() const { return nodes_; }
    size_t Count() const { return nodes_.size(); }

    // Custom pass callbacks (registered by plugins/modules).
    using PassFn = std::function<void(const RenderObject*)>;
    Result<void> RegisterPassFn(std::string_view nodeId, PassFn fn);
    // Runs all registered pass functions in topological order.
    void ExecutePasses() const;

private:
    std::vector<GraphNode> nodes_;
    std::map<std::string, PassFn, std::less<>> passFns_;
};

} // namespace bps::rendering
