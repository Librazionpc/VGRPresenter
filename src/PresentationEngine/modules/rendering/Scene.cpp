#include "modules/rendering/Scene.hpp"

#include <algorithm>
#include <cmath>

namespace bps::rendering {

// ---------------------------------------------------------------------------
// SceneNode
// ---------------------------------------------------------------------------

SceneNode* SceneNode::AddChild(std::shared_ptr<SceneNode> child) {
    if (!child) return nullptr;
    child->parent_ = this;
    children_.push_back(std::move(child));
    return children_.back().get();
}

bool SceneNode::RemoveChild(std::string_view childId) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if ((*it)->id_ == childId) {
            children_.erase(it);
            return true;
        }
    }
    return false;
}

Transform SceneNode::World() const {
    Transform w = local_;
    if (!parent_) return w;
    Transform pw = parent_->World();
    // Compose: world = parentPos + localPos (rotated+scaled), rotation sums.
    const float cosR = std::cos(pw.rotationRad), sinR = std::sin(pw.rotationRad);
    const float lx = local_.position.x * pw.scale.x;
    const float ly = local_.position.y * pw.scale.y;
    w.position.x = pw.position.x + lx * cosR - ly * sinR;
    w.position.y = pw.position.y + lx * sinR + ly * cosR;
    w.rotationRad = pw.rotationRad + local_.rotationRad;
    w.scale.x = pw.scale.x * local_.scale.x;
    w.scale.y = pw.scale.y * local_.scale.y;
    return w;
}

void SceneNode::AddComponent(std::shared_ptr<Component> c) {
    if (c) components_.push_back(std::move(c));
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

Result<void> Scene::AddLayer(const Layer& layer) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& l : layers_)
        if (l.id == layer.id)
            return Error::Make(Err::AlreadyExists, "Scene",
                               "layer '" + layer.id + "' exists");
    layers_.push_back(layer);
    std::stable_sort(layers_.begin(), layers_.end(),
                     [](const Layer& a, const Layer& b) { return a.order < b.order; });
    return Ok();
}

Result<void> Scene::RemoveLayer(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = layers_.begin(); it != layers_.end(); ++it) {
        if (it->id == id) {
            layers_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Render_LayerNotFound, "Scene",
                       "layer '" + std::string(id) + "' not found");
}

Layer* Scene::FindLayer(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& l : layers_)
        if (l.id == id) return &l;
    return nullptr;
}

std::vector<Layer> Scene::LayersSorted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Layer> out = layers_;
    std::stable_sort(out.begin(), out.end(),
                     [](const Layer& a, const Layer& b) { return a.order < b.order; });
    return out;
}

void Scene::SetLayerEnabled(std::string_view id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& l : layers_)
        if (l.id == id) {
            l.enabled = enabled;
            return;
        }
}

SceneNode* Scene::FindNode(std::string_view id) const {
    return root_ ? FindNodeRec(root_.get(), id) : nullptr;
}

SceneNode* Scene::FindNodeRec(SceneNode* n, std::string_view id) const {
    if (!n) return nullptr;
    if (n->Id() == id) return n;
    for (const auto& c : n->Children()) {
        if (auto* found = FindNodeRec(c.get(), id)) return found;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// SceneGraph
// ---------------------------------------------------------------------------

Result<std::shared_ptr<Scene>> SceneGraph::CreateScene(std::string_view id,
                                                       std::string_view name, Size size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scenes_.find(id) != scenes_.end())
        return Error::Make(Err::AlreadyExists, "SceneGraph",
                           "scene '" + std::string(id) + "' exists");
    auto scene = std::make_shared<Scene>(std::string(id), std::string(name), size);
    scenes_[std::string(id)] = scene;
    return scene;
}

Result<std::shared_ptr<Scene>> SceneGraph::GetScene(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = scenes_.find(id);
    if (it == scenes_.end())
        return Error::Make(Err::Render_SceneNotFound, "SceneGraph",
                           "scene '" + std::string(id) + "' not found");
    return it->second;
}

Result<void> SceneGraph::DestroyScene(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scenes_.erase(std::string(id)) == 0)
        return Error::Make(Err::Render_SceneNotFound, "SceneGraph",
                           "scene '" + std::string(id) + "' not found");
    return Ok();
}

std::vector<std::shared_ptr<Scene>> SceneGraph::Scenes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<Scene>> out;
    out.reserve(scenes_.size());
    for (const auto& [id, s] : scenes_) {
        (void)id;
        out.push_back(s);
    }
    return out;
}

void SceneGraph::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    scenes_.clear();
}

} // namespace bps::rendering
