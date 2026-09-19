#pragma once

// Scene system (docs/specs/17 §Scene Manager / Scene Graph). A presentation
// *becomes* a scene: Scene → Layers → Objects → Components. Everything is a
// node in a hierarchy; nodes carry transforms and components. The renderer
// knows nothing about documents — only scenes.

#include "core/common/Common.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {

class Scene;

// --- Layer kinds (independent render layers; any may be disabled) -----------
enum class LayerKind : int {
    Background = 0,
    Video,
    Image,
    Text,
    Overlay,
    Debug,
    Custom,
};

inline const char* ToString(LayerKind k) {
    switch (k) {
        case LayerKind::Background: return "Background";
        case LayerKind::Video:      return "Video";
        case LayerKind::Image:      return "Image";
        case LayerKind::Text:       return "Text";
        case LayerKind::Overlay:    return "Overlay";
        case LayerKind::Debug:      return "Debug";
        case LayerKind::Custom:     return "Custom";
    }
    return "Unknown";
}

struct Layer {
    std::string id;
    std::string name;
    LayerKind kind = LayerKind::Image;
    int order = 0;          // lower renders first
    bool enabled = true;
    float opacity = 1.0f;
    Layer() = default;
    Layer(std::string i, std::string n, LayerKind k, int o)
        : id(std::move(i)), name(std::move(n)), kind(k), order(o) {}
};

// --- Camera (orthographic; perspective is a future extension) ---------------
struct Camera {
    Vec2 position;           // world offset
    float zoom = 1.0f;
    Size viewport;           // logical viewport (scene size by default)
    Rect safeArea;           // title-safe region (default = full viewport)
    Camera() = default;
    explicit Camera(Size vp) : viewport(vp) {
        safeArea = Rect(0, 0, vp.width, vp.height);
    }
};

// --- Component: attachable data on a node -------------------------------
class Component {
public:
    virtual ~Component() = default;
    virtual const char* TypeName() const noexcept = 0;
};

// --- SceneNode: one node in the scene graph -----------------------------
class SceneNode {
public:
    SceneNode() = default;
    SceneNode(std::string id, std::string name)
        : id_(std::move(id)), name_(std::move(name)) {}

    const std::string& Id() const { return id_; }
    const std::string& Name() const { return name_; }

    Transform& Local() { return local_; }
    const Transform& Local() const { return local_; }

    SceneNode* Parent() const { return parent_; }
    SceneNode* AddChild(std::shared_ptr<SceneNode> child);
    bool RemoveChild(std::string_view childId);
    const std::vector<std::shared_ptr<SceneNode>>& Children() const { return children_; }

    // World transform = parent world × local (position/rotation/scale compose).
    Transform World() const;

    // Components.
    void AddComponent(std::shared_ptr<Component> c);
    template <typename T>
    T* FindComponent() const {
        for (const auto& c : components_)
            if (dynamic_cast<T*>(c.get())) return static_cast<T*>(c.get());
        return nullptr;
    }
    const std::vector<std::shared_ptr<Component>>& Components() const {
        return components_;
    }

    bool Visible() const { return visible_; }
    void SetVisible(bool v) { visible_ = v; }
    float Opacity() const { return opacity_; }
    void SetOpacity(float o) { opacity_ = o; }

private:
    std::string id_;
    std::string name_;
    Transform local_;
    SceneNode* parent_ = nullptr;
    std::vector<std::shared_ptr<SceneNode>> children_;
    std::vector<std::shared_ptr<Component>> components_;
    bool visible_ = true;
    float opacity_ = 1.0f;
};

// --- Scene: layers + a scene graph + a camera ---------------------------
class Scene {
public:
    Scene() = default;
    Scene(std::string id, std::string name, Size size)
        : id_(std::move(id)), name_(std::move(name)), size_(size) {
        camera_ = Camera(size);
        root_ = std::make_shared<SceneNode>("root", "Root");
    }

    const std::string& Id() const { return id_; }
    const std::string& Name() const { return name_; }
    Size SceneSize() const { return size_; }
    void SetSize(Size s) { size_ = s; }

    // Layers.
    Result<void> AddLayer(const Layer& layer);
    Result<void> RemoveLayer(std::string_view id);
    Layer* FindLayer(std::string_view id);
    std::vector<Layer> LayersSorted() const;   // by order, enabled first
    size_t LayerCount() const { return layers_.size(); }
    void SetLayerEnabled(std::string_view id, bool enabled);

    // Scene graph.
    SceneNode* Root() const { return root_.get(); }
    SceneNode* FindNode(std::string_view id) const;

    Camera& Cam() { return camera_; }
    const Camera& Cam() const { return camera_; }

    uint64_t Frame() const { return frame_; }
    void BumpFrame() { ++frame_; }

private:
    SceneNode* FindNodeRec(SceneNode* n, std::string_view id) const;

    std::string id_;
    std::string name_;
    Size size_;
    Camera camera_;
    std::shared_ptr<SceneNode> root_;
    std::vector<Layer> layers_;            // guarded by mutex_
    mutable std::mutex mutex_;
    uint64_t frame_ = 0;
};

// --- SceneGraph facade: owns scenes by id -------------------------------
class SceneGraph {
public:
    Result<std::shared_ptr<Scene>> CreateScene(std::string_view id, std::string_view name,
                                               Size size);
    Result<std::shared_ptr<Scene>> GetScene(std::string_view id) const;
    Result<void> DestroyScene(std::string_view id);
    std::vector<std::shared_ptr<Scene>> Scenes() const;
    size_t Count() const { return scenes_.size(); }
    void Clear();

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Scene>, std::less<>> scenes_;
};

} // namespace bps::rendering
