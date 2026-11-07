#pragma once

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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::rendering {
struct ElementCost {
    std::string layerId;
    std::string layerName;
    std::string objectId;
    std::string objectName;
    uint64_t pixels = 0;      // pixel work attributed to this element
    uint32_t commands = 0;    // how many draw commands it emitted
    double sharePct = 0.0;    // pixels / the frame's total pixel work
    double estimatedMs = 0.0; // sharePct of the frame's measured cost
    bool isBackground = false;

    // "Images · Announcement video" — what a human reads in a log or a caption.
    std::string Label() const {
        if (layerName.empty() && objectName.empty())
            return objectId;
        if (objectName.empty())
            return layerName;
        if (layerName.empty())
            return objectName;
        return layerName + " · " + objectName;
    }
};

struct FrameCostProfile {
    uint64_t frame = 0;         // which frame this profile describes
    double frameMs = 0.0;       // the measured cost of that frame
    uint64_t totalPixels = 0;   // the frame's whole pixel work
    // Heaviest first, capped at kProfileItems (the ones worth naming; the rest
    // are folded into nothing rather than listed as noise).
    std::vector<ElementCost> items;

    // The heaviest element of the frame (background clear included), when there
    // is one.
    const ElementCost* Heaviest() const { return items.empty() ? nullptr : &items.front(); }
    const ElementCost* HeaviestContent() const;
    // How much of the frame the background clear took, so a content share is not
    // read as the whole story (0 when the frame had no clear of its own).
    double BackgroundSharePct() const;
    static constexpr size_t kProfileItems = 8;
};

struct RenderOptions {
    bool clearBefore = true;
    Color clearColor{0, 0, 0, 1};
    bool applyAnimations = true;
    double timeSec = 0.0;      // explicit time (else engine clock used)
    bool distribute = true;    // feed enabled outputs
    bool capture = false;      // also return the frame (preview/thumbnail path)
    // A DISTRIBUTING render returns the frame it composed unless this is false: false moves the
    // pixels straight into the shared frame the outputs hold and returns none (the live loop reads
    // the picture back through LastDistributed(), so nothing copies 8 MB).
    bool keepFrame = true;
    SlideFilterValues filters;

    float contentScrollChars = 0.0f;
    bool contentScrollFromScene = false;
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

    std::string ActiveBackendName() const;
    std::string RequestedBackendName() const;

    bool Initialized() const { return initialized_.load(); }

    static std::string ResolveBackendName(std::string_view explicitBackend, bool hardwareAcceleration);

    FrameCostProfile CostProfileFor(std::string_view sceneId) const;
    FrameCostProfile LastCostProfile() const;

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
    Result<void> RemoveObject(std::string_view sceneId, std::string_view objId);

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

    const FontManager& Fonts() const noexcept { return fonts_; }

    Result<void> AddOutput(std::shared_ptr<IRenderOutput> output);
    Result<std::shared_ptr<IRenderOutput>> GetOutput(std::string_view name) const;
    Result<void> RemoveOutput(std::string_view name);
    size_t OutputCount() const;
    void Distribute(const Frame& frame) { outputs_.Distribute(frame); }
    void Distribute(const std::shared_ptr<const Frame>& frame) { outputs_.Distribute(frame); }
    // The frame the last DISTRIBUTING Render() handed to the outputs: shared and immutable, so the live
    // loop keeps it for re-presenting without a copy of its own. (A render that distributes and does
    // not capture no longer returns its pixels at all — this is where they are.)
    std::shared_ptr<const Frame> LastDistributed() const {
        std::lock_guard<std::mutex> lock(lastDistributedMutex_);
        return lastDistributed_;
    }

    // ---- Feed health (the automatic blackout's inputs) --------------------
    struct OutputFeedAge {
        std::string name;
        int64_t ageMs = -1;   // -1 = this output has never been presented to
        bool enabled = false;
        uint64_t frames = 0;
    };
    std::vector<OutputFeedAge> OutputFeedAges() const;
    int BlackoutOutputs(const std::vector<std::string>& names);

    // --- Resources (async) ---
    Result<TextureId> UploadTexture(std::string_view name, const RgbaImage& image);
    Result<void> ReleaseTexture(std::string_view name);
    Result<TextureId> GetTexture(std::string_view name) const;
    void SubmitAsyncUpload(std::string name, std::vector<uint8_t> bytes,
                           ResourceUploader::Decoder decoder,
                           ResourceUploader::DoneFn done);

    void SetAlphaInspect(bool on) noexcept { alphaInspect_.store(on, std::memory_order_relaxed); }
    bool AlphaInspect() const noexcept { return alphaInspect_.load(std::memory_order_relaxed); }

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

    Result<void> SelectBackend(std::string_view requested);

    static std::string ConfiguredBackendName();

    IGraphicsBackend* backend_ = nullptr;
    std::unique_ptr<IGraphicsBackend> d3d11Backend_;
    std::unique_ptr<NullGraphicsBackend> nullBackend_;
    std::unique_ptr<SoftwareGraphicsBackend> softwareBackend_;
    // The one line that says which backend this process selected, and on which
    // request (see SelectBackend) — logged once per distinct answer.
    bool loggedBackendOnce_ = false;
    std::string loggedBackend_;
    mutable std::mutex backendNameMutex_;
    std::string requestedBackendName_;
    std::string activeBackendName_;

    float contentScrollChars_ = 0.0f;
    bool contentScrollFromScene_ = false;

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

    std::unordered_map<std::string, FrameCostProfile> costProfiles_;
    FrameCostProfile costLast_;

    std::vector<Subscription> subscriptions_;
    std::atomic<uint64_t> frameCounter_{0};
    // The last frame's draw-command count — see RenderStats::commands.
    std::atomic<uint32_t> lastCommandCount_{0};
    std::atomic<uint64_t> frameDrops_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<PressureLevel> pressure_{PressureLevel::None};
    // Transparency inspection (see SetAlphaInspect). Off unless asked for.
    std::atomic<bool> alphaInspect_{false};
    mutable std::chrono::steady_clock::time_point lastFrameAt_;
    double lastFrameMs_ = 0.0;
    mutable std::mutex lastDistributedMutex_;
    std::shared_ptr<const Frame> lastDistributed_;
    mutable std::mutex renderMutex_;
};
} // namespace bps::rendering