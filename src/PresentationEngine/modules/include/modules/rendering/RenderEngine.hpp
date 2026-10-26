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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::rendering {

// Frame options (per render call).
// WHERE A FRAME'S COST WENT — the answer to "the show is getting slower, what is
// doing it?". The renderer already knows the command list it built and which
// object each command was built for, so it can account for the PIXEL WORK every
// element asked for (a fill or a blit is its destination area, a scaled blit its
// sampled source area — see CommandWork) and hand back the elements heaviest
// first. `sharePct` is that element's part of the frame's total work, and
// `estimatedMs` is the share of the frame's MEASURED cost that the work implies
// — an estimate by construction (the measurement is the frame, the split is the
// work), which is exactly what "which layer made this frame expensive" needs,
// and never a claim that a stopwatch ran per element.
struct ElementCost {
    std::string layerId;
    std::string layerName;
    std::string objectId;
    std::string objectName;
    uint64_t pixels = 0;      // pixel work attributed to this element
    uint32_t commands = 0;    // how many draw commands it emitted
    double sharePct = 0.0;    // pixels / the frame's total pixel work
    double estimatedMs = 0.0; // sharePct of the frame's measured cost
    // The scene's own clear, not a layer anyone authored. It is real work (a
    // full-resolution fill) but it is the SAME work in every frame of a scene,
    // so it belongs in the account (the shares have to add up) and must not be
    // the element a slowdown report NAMES — see HeaviestContent().
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
    // THE ONE TO NAME: the heaviest element that is not the scene's own clear.
    // A full-frame clear outranks everything in a normal scene (it is a whole
    // frame's worth of pixels before any layer draws), so naming Heaviest()
    // would report "Background clear" in every second of every service — true
    // and useless. Null for a frame that had nothing on it but its background.
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
    // Slide-WIDE filters applied to the FINISHED frame, after readback and
    // before the frame is captured or distributed (see
    // Effects.hpp::ApplyFilters). This is where they have to live: the frame
    // that leaves here is the one every output receives, so filtering here is
    // what makes the HDMI/NDI feed, the preview buffers and the monitor tiles
    // show the same picture — filtering in the caller could only ever reach the
    // copies it made itself. Identity by default (the ordinary slide pays
    // nothing: ApplyFilters returns immediately).
    SlideFilterValues filters;

    // THE READING SCROLL (the Alt+Down / Alt+Up reading step).
    //
    // Where the scene's READING text sits inside its own flow, in LINE HEIGHTS
    // of that text (0 = the top of the passage; an ordinary frame passes the
    // resting value, which is not necessarily 0). The reading block holds the
    // WHOLE passage set below its page (see TextObject::SetReadingFlow), so a
    // scroll is a window: this number slides that one layout under the block's
    // box and the box clips it. Nothing is re-laid out, no string is rebuilt,
    // so the type size, the wrapping and the design's geometry are the same at
    // every offset — which is what makes the move a scroll instead of a
    // succession of re-fitted snapshots.
    //
    // ONE number drives every surface. The live loop stores it from the
    // interface thread's animator (LiveOutputController::SetLineScroll), so the
    // HDMI window, the NDI feed, the recordings and the QML monitor tile — which
    // animates the same offset itself, from the same settings — travel together.
    //
    // In LINE units, not pixels, because a line height is the text's own
    // (`TextEngine` advances every line by size + lineSpacing). Applied to the
    // ONE object that carries a flow: a title, a reference, a clock, the design's
    // shapes and its background stay exactly where they are — the words travel
    // inside their block, they do not drag the slide.
    float contentScrollLines = 0.0f;
    // Is this the FIRST frame of a freshly built scene? Then the offset comes
    // from the scene itself (Slide::flowScroll, carried on the text object) and
    // `contentScrollLines` above is ignored — so a page change lands at the
    // offset the slide was committed with, without waiting for the interface
    // thread's next animation tick. On every later frame the pushed number
    // wins, which is what keeps an in-page step (no rebuild, no new slide) at
    // the offset it just moved to.
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

    // WHAT IS DRAWING, AND WHAT WAS ASKED FOR — the two questions the app asks
    // after a user flips Settings · General's "Hardware rendering (GPU)".
    //
    //   ActiveBackendName()     the backend that is really drawing: "software",
    //                           "directx11", "null" ("" before the first
    //                           selection). Asked for a GPU on a machine that has
    //                           none, this is "software" — the fallback, and the
    //                           fact the app reports instead of an assumption
    //                           about the switch.
    //   RequestedBackendName()  the name the last selection was made with.
    //
    // Neither touches a device: they are strings the selection left behind, so
    // the interface thread may ask (and it must not be able to wait for a
    // frame — see SelectBackend).
    std::string ActiveBackendName() const;
    std::string RequestedBackendName() const;

    // Has Initialize() run? The settings layer pushes the user's renderer choice
    // at the engine at boot, and an engine that has not initialized yet has no
    // backends to re-bind — the value it writes is read by the Initialize that
    // follows instead (see SettingsService's applyRenderBackendPreference).
    bool Initialized() const { return initialized_.load(); }

    // The backend a selection resolves to, from the two things that may ask for
    // one — `render.backend` (an explicit choice, which WINS) or else the app's
    // hardware-acceleration preference — and the software rasterizer when
    // neither asks for the GPU. Pure and static so the rule is pinable without a
    // GPU and without a config store (see the .cpp for why software is the
    // default).
    static std::string ResolveBackendName(std::string_view explicitBackend, bool hardwareAcceleration);

    // Where a frame's cost went (see FrameCostProfile). Both are copied out under
    // the render lock, so a caller on another thread (the live loop publishes it
    // to Telemetry) reads a whole profile and never a half-updated one.
    //   CostProfileFor(sceneId) — the last frame of THAT scene, which is the one
    //     the live loop wants: a per-output pass may have been the most recent
    //     render of the frame, and the composed program scene is the expensive
    //     one it is asking about.
    //   LastCostProfile() — the most recent frame of any scene.
    // Empty until a frame has been rendered.
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
    // Detaches objId from sceneId's scene graph (SceneNode::RemoveChild
    // under the hood). For a CACHED scene whose object set can change
    // between renders without the scene itself being rebuilt (e.g.
    // SceneBuilder's compositor layers — a taken input/media frame updates
    // via GetObject+mutate-in-place instead, but an overlay taken/cleared
    // mid-slide needs an object actually gone, not just re-added on top of
    // a stale one AddObject would otherwise duplicate).
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

    // --- Outputs ---
    // Read access to the font manager — scene builders (SceneBuilder's
    // autoSize:"shrink" fit solver) measure text with the SAME font stack
    // DrawTextObject rasterizes with, so fitted sizes match what gets drawn.
    const FontManager& Fonts() const noexcept { return fonts_; }

    Result<void> AddOutput(std::shared_ptr<IRenderOutput> output);
    Result<std::shared_ptr<IRenderOutput>> GetOutput(std::string_view name) const;
    Result<void> RemoveOutput(std::string_view name);
    size_t OutputCount() const;
    // Hands an ALREADY COMPOSED frame to the outputs (scaled to each one's own
    // target size by Present) WITHOUT rasterizing anything again. This is the
    // cheap half of the live loop's dirty-frame path: when the on-air scene has
    // not changed there is nothing to rasterize, but the outputs, the NDI
    // sender, the recorders and the output meter all read presents — so the
    // composed frame is handed over again instead of being re-drawn per output.
    // Goes through the same OutputManager::Distribute the render path uses, so
    // the telemetry output meter counts these presents exactly as it always has.
    void Distribute(const Frame& frame) { outputs_.Distribute(frame); }

    // ---- Feed health (the automatic blackout's inputs) --------------------
    // HOW LONG AGO each output last received a frame, in milliseconds, with
    // -1 for an output that has never been presented to. The live loop's 1 Hz
    // heartbeat re-presents a static picture, so these ages advance about once
    // a second on every enabled output while a session is healthy — an age that
    // keeps climbing is a stalled feed, not an unchanged slide (see
    // IRenderOutput::LastPresentSteadyMs). A plain read: names and atomic
    // stamps under the manager's lock, no device and no frame copy.
    struct OutputFeedAge {
        std::string name;
        int64_t ageMs = -1;   // -1 = this output has never been presented to
        bool enabled = false;
        // HOW MANY FRAMES THIS OUTPUT HAS EVER RECEIVED (IRenderOutput::
        // FramesReceived — the same number the NDI sender reports, for every
        // kind of output). It is what lets the Outputs screen say the same
        // thing about an HDMI row as about an NDI row: "N frames delivered"
        // is proof the picture reached THIS output, not a label. Per buffer,
        // so two rows that mirror the shared preview feed report the same
        // count — which is exactly the truth about them.
        uint64_t frames = 0;
    };
    std::vector<OutputFeedAge> OutputFeedAges() const;
    // Present an opaque BLACK frame to the named outputs — what a live output
    // whose feed has stalled puts up instead of holding its last picture (a
    // frozen frame reads as a live feed, which is the one thing it must not
    // do). Nothing needs to be undone: the live loop's next present overwrites
    // it the moment frames flow again, so recovery is automatic. Returns how
    // many outputs were blacked.
    int BlackoutOutputs(const std::vector<std::string>& names);

    // --- Resources (async) ---
    Result<TextureId> UploadTexture(std::string_view name, const RgbaImage& image);
    Result<void> ReleaseTexture(std::string_view name);
    Result<TextureId> GetTexture(std::string_view name) const;
    void SubmitAsyncUpload(std::string name, std::vector<uint8_t> bytes,
                           ResourceUploader::Decoder decoder,
                           ResourceUploader::DoneFn done);

    // --- Transparency inspection (diagnostic) ---
    // When ON, every CAPTURED/DISTRIBUTED frame composites its transparent
    // pixels over a checkerboard, so a keyed feed's alpha is VISIBLE on the
    // real bound output window, the NDI feed and HDMI — not only in the in-app
    // monitor tile (which draws its own QML checkerboard). Opaque pixels are
    // left untouched (alpha 255 composites to itself), so a fully opaque scene
    // is bit-identical either way. DIAGNOSTIC ONLY: it repaints the frame's
    // alpha, so it must stay OFF for a live keyed feed. See VGR_ALPHA_INSPECT.
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

    // Installs the backend named by `render.backend` ("auto" = the GPU when one
    // can really be made, software otherwise; see the .cpp for the contract,
    // including the fallback and the log line that says which one is live).
    Result<void> SelectBackend(std::string_view requested);

    // What the configuration asks for right now (ResolveBackendName over
    // `render.backend` and `render.hardwareAcceleration`) — the one place
    // Initialize and Reload read the request from, so a boot and a switch after
    // it cannot answer differently.
    static std::string ConfiguredBackendName();

    // The backends, in the order the engine prefers them: the D3D11 GPU one
    // exists only when a device could really be made, and stays null on a
    // build/machine that has none — so such a machine runs the software
    // rasterizer exactly as it did before. The unique_ptr<IGraphicsBackend>
    // form is what keeps <d3d11.h> out of this header.
    IGraphicsBackend* backend_ = nullptr;
    std::unique_ptr<IGraphicsBackend> d3d11Backend_;
    std::unique_ptr<NullGraphicsBackend> nullBackend_;
    std::unique_ptr<SoftwareGraphicsBackend> softwareBackend_;
    // The one line that says which backend this process selected, and on which
    // request (see SelectBackend) — logged once per distinct answer.
    bool loggedBackendOnce_ = false;
    std::string loggedBackend_;
    // The pair the two accessors above read, under their own lock rather than
    // renderMutex_ on purpose: SelectBackend runs on the settings worker while
    // the render thread may be mid-frame, and the only thing shared is these two
    // strings — nothing here may wait for a frame to finish, and a frame never
    // waits for this.
    mutable std::mutex backendNameMutex_;
    std::string requestedBackendName_;
    std::string activeBackendName_;

    // THE READING SCROLL for the frame being drawn (see
    // RenderOptions::contentScrollLines): how far the reading block's flow is
    // slid, in its own line heights. Set at the top of Render under renderMutex_
    // and cleared at the end of it, so a frame can never leave the shared scene
    // in a moved state — the same scene is rasterised again by every other
    // output's pass.
    float contentScrollLines_ = 0.0f;
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

    // The frame cost accounting (see FrameCostProfile): written by the render
    // thread inside Render, read through the two accessors above under
    // renderMutex_.
    std::unordered_map<std::string, FrameCostProfile> costProfiles_;
    FrameCostProfile costLast_;

    std::vector<Subscription> subscriptions_;
    std::atomic<uint64_t> frameCounter_{0};
    std::atomic<uint64_t> frameDrops_{0};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<PressureLevel> pressure_{PressureLevel::None};
    // Transparency inspection (see SetAlphaInspect). Off unless asked for.
    std::atomic<bool> alphaInspect_{false};
    mutable std::chrono::steady_clock::time_point lastFrameAt_;
    double lastFrameMs_ = 0.0;
    // Serializes the whole frame sequence (collect → draw → submit → readback)
    // and diagnostics access. RenderEngine is a singleton reachable from the
    // scheduler tick, event threads and modules; one frame at a time keeps the
    // software backend's framebuffer + lazy texture uploads consistent.
    mutable std::mutex renderMutex_;
};

} // namespace bps::rendering
