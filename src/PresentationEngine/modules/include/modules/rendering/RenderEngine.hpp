#pragma once

// RenderEngine (docs/specs/17): the facade every module uses to render. Owns
// scenes, the pipeline, the render graph, backends, outputs, animation/
// transition engines and diagnostics. Knows nothing about presentations or
// frontends. Renders scenes → layers → objects → pixels and distributes frames
// to Render Outputs.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/rendering/Animation.hpp"
#include "modules/rendering/Effects.hpp"
#include "modules/rendering/GpuResources.hpp"
#include "modules/rendering/GraphicsBackends.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"
#include "modules/rendering/Pipeline.hpp"
#include "modules/rendering/RenderObject.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/Scene.hpp"
#include "modules/rendering/TextEngine.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {

// Frame options (per render call).
struct RenderOptions {
    bool clearBefore = true;
    Color clearColor{0, 0, 0, 1};
    bool applyAnimations = true;
    double timeSec = 0.0;      // explicit time (else engine clock used)
    bool distribute = true;    // feed enabled outputs
    bool capture = false;      // also return the frame (preview/thumbnail path)
};

class RenderEngine final : public IService {
public:
    static RenderEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "RenderEngine"; }

    // --- Backend selection ---
    // Installs the active backend (default: software). Null keeps headless.
    Result<void> SetBackend(std::string_view name);
    const BackendCapabilities& BackendCaps() const;
    IGraphicsBackend* Backend() const { return backend_; }

    // --- Scenes ---
    Result<std::shared_ptr<Scene>> CreateScene(std::string_view id, std::string_view name,
                                               Size size);
    Result<std::shared_ptr<Scene>> GetScene(std::string_view id) const;
    Result<void> DestroyScene(std::string_view id);

    // --- Objects (convenience builders attach to a scene graph) ---
    Result<RenderObject*> AddObject(std::string_view sceneId, std::shared_ptr<RenderObject> obj,
                                    std::string_view layerId);
    Result<RenderObject*> GetObject(std::string_view sceneId, std::string_view objId) const;
    std::vector<RenderObject*> CollectObjects(std::string_view sceneId) const;

    // --- Layers ---
    Result<void> AddLayer(std::string_view sceneId, const Layer& layer);

    // --- Animation / transitions ---
    Animator& Anim() { return animator_; }
    TransitionEngine& Transitions() { return transitions_; }

    // --- Render ---
    // Renders one frame of a scene through the pipeline into the backend.
    Result<RgbaImage> Render(std::string_view sceneId, const RenderOptions& opts = {});
    // Renders offscreen at a given size (preview/thumbnail/screenshot).
    Result<RgbaImage> RenderOffscreen(std::string_view sceneId, Size size,
                                      const RenderOptions& opts = {});
    // Renders a thumbnail (small, cheap) of the scene.
    Result<RgbaImage> RenderThumbnail(std::string_view sceneId, Size size);

    // --- Outputs ---
    Result<void> AddOutput(std::shared_ptr<IRenderOutput> output);
    Result<std::shared_ptr<IRenderOutput>> GetOutput(std::string_view name) const;
    Result<void> RemoveOutput(std::string_view name);
    size_t OutputCount() const;

    // --- Resources (async) ---
    Result<TextureId> UploadTexture(std::string_view name, const RgbaImage& image);
    Result<void> ReleaseTexture(std::string_view name);
    Result<TextureId> GetTexture(std::string_view name) const;
    void SubmitAsyncUpload(std::string name, std::vector<uint8_t> bytes,
                           ResourceUploader::Decoder decoder,
                           ResourceUploader::DoneFn done);

    // --- Diagnostics ---
    RenderStats Stats() const;
    void ResetStats();

    // --- Memory pressure reaction (called by events + ResourceManager) ---
    void OnMemoryPressure(PressureLevel level);

private:
    RenderEngine() = default;
    void WireEvents();
    void UnwireEvents();
    void OnAssetLoaded(const events::ContentAssetLoaded& e);
    void OnAssetDeleted(const events::ContentAssetDeleted& e);
    void OnDisplayChanged(const events::DisplayChanged& e);
    void OnConfigReload(const events::ConfigHotReload& e);
    void OnPressure(const events::ResourcePressureChanged& e);

    // Draw one object into the command list (called by the pipeline passes).
    void DrawObject(RenderObject* obj, std::vector<DrawCommand>& cmds);
    void DrawTextObject(TextObject* obj, std::vector<DrawCommand>& cmds);

    IGraphicsBackend* backend_ = nullptr;
    std::unique_ptr<NullGraphicsBackend> nullBackend_;
    std::unique_ptr<SoftwareGraphicsBackend> softwareBackend_;

    SceneGraph scenes_;
    RenderPipeline pipeline_;
    RenderGraph graph_;
    OutputManager outputs_;
    GPUResourceManager gpu_;
    RenderCache cache_;
    ResourceUploader uploader_;
    FrameGraph frameGraph_;
    FontManager fonts_;
    Animator animator_;
    TransitionEngine transitions_;

    std::vector<Subscription> subscriptions_;
    std::atomic<uint64_t> frameCounter_{0};
    std::atomic<uint64_t> frameDrops_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<PressureLevel> pressure_{PressureLevel::None};
    mutable std::chrono::steady_clock::time_point lastFrameAt_;
    double lastFrameMs_ = 0.0;
    // Serializes the whole frame sequence (collect → draw → submit → readback)
    // and diagnostics access. RenderEngine is a singleton reachable from the
    // scheduler tick, event threads and modules; one frame at a time keeps the
    // software backend's framebuffer + lazy texture uploads consistent.
    mutable std::mutex renderMutex_;
};

} // namespace bps::rendering
