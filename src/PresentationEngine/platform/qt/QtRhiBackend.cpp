// QtRhiBackend — the engine's GPU backend on Qt's rendering hardware interface (QRhi).
//
// WHY THIS EXISTS. The only GPU backend used to be Direct3D 11, so a Windows laptop drew the output
// on its GPU while a Linux or macOS laptop fell back to the CPU rasterizer. QRhi is one API over
// Direct3D 11 (Windows), Metal (macOS) and Vulkan / OpenGL (Linux), so this file is ONE implementation of
// the engine's draw commands for every OS: the same shader (platform/qt/shaders/engine.{vert,frag},
// compiled to every API's format by Qt Shader Tools at build time), the same pipelines, the same
// readback. A fix made here reaches all three platforms.
//
// WHAT IT IS. A line-for-line port of the D3D11 backend (D3D11Backend.cpp), command for command:
//   * the frame is an offscreen RGBA8 texture holding PREMULTIPLIED pixels (so the blend unit can do
//     source-over, additive and multiply), un-premultiplied on the way back to the CPU;
//   * Clear / FillRect / BlitImage / DrawGlyph / DrawShape are one quad each, with the D3D11 backend's
//     exact pixel-edge snapping, so abutting fills share a boundary and the picture matches the CPU
//     rasterizer's;
//   * the CSS blend modes the fixed blend unit cannot do (screen, overlay, hue ...) are a second pass
//     against a copy of the frame; region effects (blur, brightness, contrast, saturation, opacity) are
//     a copy plus a scissored pass. Same formulas as the CPU rasterizer and the D3D11 shader.
// Verified the way the D3D11 backend is: tests_rendering.cpp renders one scene on the CPU rasterizer
// and on this backend and demands the pixels agree.
//
// THREADS. A QRhi (and above all an OpenGL context) belongs to one thread at a time, while the engine
// calls its backend from the render thread, the texture uploader and the thumbnail workers. So every
// call is marshalled onto ONE dedicated GPU thread that owns the QRhi for its whole life and the caller
// blocks for the answer — the same serialisation the D3D11 backend gets from its mutex, but correct for
// APIs whose contexts are thread-affine.
//
// FRAME ORIENTATION. Commands are authored top-left origin. The vertex shader emits GL-style clip space
// and QRhi's clipSpaceCorrMatrix() adapts it per API, so the picture is the right way up everywhere;
// only OpenGL stores a render target bottom-up in memory, which the shader (frame copies) and Readback
// (the final rows) undo, keyed on QRhi::isYUpInFramebuffer().

#include "modules/rendering/RhiBackend.hpp"

#include "core/logging/Logger.hpp"
#include "modules/rendering/RenderTypes.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(BPS_HAVE_RHI) && BPS_HAVE_RHI

#include <QtGui/qtguiglobal.h>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QOffscreenSurface>
#include <QMetaObject>
#include <QSize>
#include <QThread>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

// Which GPU APIs this Qt was built with (the same conditions Qt's own qrhi_platform.h uses).
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#  define BPS_RHI_VULKAN 1
#  include <QVulkanInstance>
#endif
#if QT_CONFIG(opengl)
#  define BPS_RHI_OPENGL 1
#endif

// Must be called from the global namespace (the macro declares an extern function).
static void BpsInitRhiShaderResources() { Q_INIT_RESOURCE(bps_rhi_shaders); }

namespace bps::rendering {
namespace {

constexpr const char* kModule = "Graphics";

// ---------------------------------------------------------------------------------------------
// The uniform block (platform/qt/shaders/engine.vert: `Draw`), std140: a mat4 then seven vec4 rows.
// ---------------------------------------------------------------------------------------------
struct DrawConstants {
    float clip[16];
    float rect[4];     // x, y, w, h (destination pixels)
    float uv[4];       // u0, v0, u1, v1
    float color[4];    // straight fill colour / texture tint
    float params[4];   // x = corner radius, y = effect id, z = effect param, w = mode
    float frame[4];    // x,y = frame size; z/w = blend pass: mode, flags
    float dir[4];      // xy = one blur step, z = blur radius
    float misc[4];     // x = 1 when a render-target copy is stored bottom-up (OpenGL)
};
static_assert(sizeof(DrawConstants) == 64 + 7 * 16, "DrawConstants must match the std140 block");

// The shader's mode numbers (engine.frag).
constexpr int kModeSolid = 0;
constexpr int kModeTextured = 1;
constexpr int kModeRounded = 2;
constexpr int kModeEffect = 3;
constexpr int kModeBlend = 4;
constexpr int kModeInvertAdd = 8;   // +8 = the colour inverted (the form MULTIPLY needs)

enum class Pipe : int { Alpha = 0, Add = 1, Mul = 2, Replace = 3 };
constexpr int kPipeCount = 4;

// Premultiplied target byte -> the straight RGBA8 the engine hands downstream.
uint32_t Unpremultiply(uint32_t px) {
    const uint32_t a = (px >> 24) & 0xFFu;
    if (a == 0u || a == 255u)
        return px;
    auto straight = [a](uint32_t v) -> uint32_t {
        const uint32_t s = (v * 255u + a / 2u) / a;
        return s > 255u ? 255u : s;
    };
    return (a << 24) | (straight((px >> 16) & 0xFFu) << 16) | (straight((px >> 8) & 0xFFu) << 8) |
           straight((px >> 0) & 0xFFu);
}

bool NeedsBlendPass(BlendMode m) {
    return m != BlendMode::Alpha && m != BlendMode::Additive && m != BlendMode::Multiply;
}

Pipe PipeFor(BlendMode m) {
    if (m == BlendMode::Additive) return Pipe::Add;
    if (m == BlendMode::Multiply) return Pipe::Mul;
    return Pipe::Alpha;
}

// ---------------------------------------------------------------------------------------------
// The GPU thread. Run() executes the callable there and returns its result; called from the GPU
// thread itself it runs inline (so backend methods may call each other).
// ---------------------------------------------------------------------------------------------
class GpuExecutor {
public:
    GpuExecutor() { thread_ = std::thread([this] { Loop(); }); }
    ~GpuExecutor() { Stop(); }
    GpuExecutor(const GpuExecutor&) = delete;
    GpuExecutor& operator=(const GpuExecutor&) = delete;

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable() && !OnThread()) thread_.join();
    }

    bool OnThread() const { return std::this_thread::get_id() == thread_.get_id(); }

    template <class F>
    auto Run(F&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        if (OnThread())
            return fn();
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return result.get();
    }

private:
    void Loop() {
        (void)platform::PlatformAccessor::Get().Threading().SetCurrentThreadName("gpu-rhi");
        // Same priority the render thread runs at: the GPU thread is on the frame's critical path.
        (void)platform::PlatformAccessor::Get().Threading().SetCurrentThreadPriority(4);
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty())
                    return;   // stop_ and drained
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::thread thread_;
};

// A QOffscreenSurface belongs to the GUI thread (Qt: create and destroy it there), but the engine
// picks its backend from wherever it is called: at start-up that is the GUI thread, and a later
// "hardware acceleration" switch arrives from a worker. So creation hops to the GUI thread and WAITS
// (the GUI thread never waits on the engine, only the other way round), and destruction is posted
// there (fire-and-forget: at shutdown the loop may be gone, and a leaked surface then is harmless).
#if defined(BPS_RHI_OPENGL)
struct QtMainThreadDeleter {
    void operator()(QOffscreenSurface* surface) const {
        if (!surface) return;
        QCoreApplication* app = QCoreApplication::instance();
        if (!app || QThread::currentThread() == app->thread())
            delete surface;
        else
            QMetaObject::invokeMethod(app, [surface] { delete surface; }, Qt::QueuedConnection);
    }
};

QOffscreenSurface* NewGlSurfaceOnMainThread() {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread())
        return QRhiGles2InitParams::newFallbackSurface();
    QOffscreenSurface* surface = nullptr;
    QMetaObject::invokeMethod(app, [&surface] { surface = QRhiGles2InitParams::newFallbackSurface(); },
                              Qt::BlockingQueuedConnection);
    return surface;
}
#else
struct QtMainThreadDeleter {
    void operator()(QOffscreenSurface* surface) const { delete surface; }
};
#endif

// What the factory hands the backend: the API to create on the GPU thread, plus the platform objects
// that API needs and that must outlive the QRhi.
struct RhiSetup {
    QRhi::Implementation impl = QRhi::Null;
    std::string api;
#if defined(BPS_RHI_VULKAN)
    std::unique_ptr<QVulkanInstance> vulkan;
#endif
    std::unique_ptr<QOffscreenSurface, QtMainThreadDeleter> glSurface;
};

// ===========================================================================================
class RhiGraphicsBackend final : public IGraphicsBackend {
public:
    explicit RhiGraphicsBackend(RhiSetup setup) : setup_(std::move(setup)) {}

    ~RhiGraphicsBackend() override {
        if (executor_) {
            executor_->Run([this] { ReleaseGpu(); });
            executor_->Stop();
        }
        // The surface and the Vulkan instance outlive the QRhi, and go on the thread that made them.
        setup_.glSurface.reset();
#if defined(BPS_RHI_VULKAN)
        setup_.vulkan.reset();
#endif
    }

    // The GPU's name as the driver reports it (for the log).
    const std::string& Driver() const { return driverName_; }

    // Brings up the device and the pipeline on the GPU thread.
    bool Init(std::string* why) {
        executor_ = std::make_unique<GpuExecutor>();
        std::string reason;
        const bool ok = executor_->Run([&] { return InitOnGpuThread(&reason); });
        if (!ok && why) *why = reason;
        return ok;
    }

    const BackendCapabilities& Capabilities() const noexcept override { return caps_; }

    // ---- Textures ------------------------------------------------------------------------------
    Result<TextureId> CreateTexture(const RgbaImage& image) override {
        return CreateTextureShared(std::make_shared<const RgbaImage>(image));
    }

    Result<TextureId> CreateTextureShared(const SharedRgbaImage& image) override {
        if (!image || image.get()->empty())
            return Error::Make(Err::InvalidArgument, "Rhi", "empty texture");
        return executor_->Run([&]() -> Result<TextureId> {
            if (!rhi_)
                return Error::Make(Err::Render_BackendUnavailable, "Rhi", "no device");
            Texture t;
            if (!MakeTexture(*image, t))
                return Error::Make(Err::Render_BackendUnavailable, "Rhi", "texture creation failed");
            t.cpu = image;
            t.pending = image;
            const TextureId id{nextTexture_++};
            textureBytes_ += image->ByteSize();
            textures_.emplace(id.id, std::move(t));
            return id;
        });
    }

    Result<void> UpdateTexture(TextureId id, const RgbaImage& image) override {
        return UpdateTextureShared(id, std::make_shared<const RgbaImage>(image));
    }

    Result<void> UpdateTextureShared(TextureId id, const SharedRgbaImage& image) override {
        return executor_->Run([&]() -> Result<void> {
            auto it = textures_.find(id.id);
            if (it == textures_.end())
                return Error::Make(Err::Render_TextureNotFound, "Rhi", "no texture");
            if (!image || image.get()->empty() || !rhi_)
                return Ok();
            Texture& t = it->second;
            if (!t.cpu || t.cpu->width != image->width || t.cpu->height != image->height) {
                // A size change is a new GPU texture (and new bindings that name it).
                DropBindingsFor(t.tex.get());
                Texture fresh;
                if (!MakeTexture(*image, fresh))
                    return Error::Make(Err::Render_BackendUnavailable, "Rhi",
                                       "texture re-creation failed after a size change");
                if (t.cpu) textureBytes_ -= t.cpu->ByteSize();
                t = std::move(fresh);
                textureBytes_ += image->ByteSize();
            } else {
                textureBytes_ -= t.cpu->ByteSize();
                textureBytes_ += image->ByteSize();
            }
            t.cpu = image;
            t.pending = image;   // uploaded at the start of the next frame, with everything else
            return Ok();
        });
    }

    Result<void> DestroyTexture(TextureId id) override {
        return executor_->Run([&]() -> Result<void> {
            auto it = textures_.find(id.id);
            if (it == textures_.end())
                return Error::Make(Err::Render_TextureNotFound, "Rhi", "no texture");
            DropBindingsFor(it->second.tex.get());
            if (it->second.cpu) textureBytes_ -= it->second.cpu->ByteSize();
            textures_.erase(it);
            return Ok();
        });
    }

    RgbaImage ReadTexture(TextureId id) const override {
        return const_cast<RhiGraphicsBackend*>(this)->executor_->Run([&]() -> RgbaImage {
            auto it = textures_.find(id.id);
            return (it == textures_.end() || !it->second.cpu) ? RgbaImage{} : *it->second.cpu;
        });
    }

    uint64_t TextureMemoryBytes() const noexcept override { return textureBytes_.load(); }

    // ---- Shaders (recorded; the engine's own pipeline is fixed) ----------------------------------
    Result<uint32_t> CreateShader(std::string_view, std::string_view source) override {
        std::lock_guard<std::mutex> lock(shaderMutex_);
        const uint32_t id = nextShader_++;
        shaders_[id] = std::string(source);
        return id;
    }
    Result<void> DestroyShader(uint32_t id) override {
        std::lock_guard<std::mutex> lock(shaderMutex_);
        shaders_.erase(id);
        return Ok();
    }
    size_t ShaderCount() const noexcept override {
        std::lock_guard<std::mutex> lock(shaderMutex_);
        return shaders_.size();
    }

    // ---- Frame lifecycle -------------------------------------------------------------------------
    Result<void> BeginFrame(int width, int height) override {
        if (width <= 0 || height <= 0)
            return Error::Make(Err::InvalidArgument, "Rhi", "bad frame size");
        return executor_->Run([&]() -> Result<void> {
            if (!rhi_)
                return Error::Make(Err::Render_BackendUnavailable, "Rhi", "no device");
            if (!EnsureFrame(width, height))
                return Error::Make(Err::Render_BackendUnavailable, "Rhi", "frame targets");
            pending_ = PendingEffect{};
            return Ok();
        });
    }

    Result<void> Submit(const std::vector<DrawCommand>& commands) override {
        return executor_->Run([&]() -> Result<void> { return SubmitOnGpuThread(commands); });
    }

    Result<RgbaImage> Readback() const override {
        return const_cast<RhiGraphicsBackend*>(this)->executor_->Run(
            [&]() -> Result<RgbaImage> { return const_cast<RhiGraphicsBackend*>(this)->ReadbackOnGpuThread(); });
    }

    Result<void> Present() override { return Ok(); }    // offscreen: nothing to present
    Result<void> EndFrame() override { return Ok(); }

    Result<void> Shutdown() override {
        executor_->Run([this] { ReleaseGpu(); });
        return Ok();
    }

private:
    // ---- the GPU-thread state --------------------------------------------------------------------
    struct Texture {
        std::unique_ptr<QRhiTexture> tex;
        SharedRgbaImage cpu;       // what ReadTexture hands back; also the size the GPU texture has
        SharedRgbaImage pending;   // the picture the next frame must upload (null = GPU copy is current)
    };

    struct PendingEffect {
        bool active = false;
        Rect region;
        int effect = 0;
        float param = 0.0f;
    };

    struct IRect { int x = 0, y = 0, w = 0, h = 0; };

    // One step of a frame, already translated from the engine's commands.
    struct Op {
        enum class Kind { Draw, CopyFrame } kind = Kind::Draw;
        Pipe pipe = Pipe::Alpha;
        QRhiTexture* t1 = nullptr;       // slot 1: the picture / effect source (the white dummy when none)
        QRhiTexture* t2 = nullptr;       // slot 2: the blend pass's backdrop copy (the white dummy when none)
        uint32_t offset = 0;             // byte offset of this draw's constants in the uniform buffer
        IRect scissor;
        QRhiTexture* copyTo = nullptr;   // CopyFrame: the scratch texture that receives the frame
    };

    struct Plan {
        std::vector<Op> ops;
        std::vector<DrawConstants> constants;
        size_t draws = 0;
    };

    // ---------------------------------------------------------------------------------------------
    bool InitOnGpuThread(std::string* why) {
        BpsInitRhiShaderResources();

        // ---- the device ----
        QRhi::Flags flags = QRhi::SuppressSmokeTestWarnings;
        switch (setup_.impl) {
#if defined(Q_OS_WIN)
        case QRhi::D3D11: {
            QRhiD3D11InitParams params;
            rhi_.reset(QRhi::create(QRhi::D3D11, &params, flags));
            break;
        }
#endif
#if defined(Q_OS_MACOS)
        case QRhi::Metal: {
            QRhiMetalInitParams params;
            rhi_.reset(QRhi::create(QRhi::Metal, &params, flags));
            break;
        }
#endif
#if defined(BPS_RHI_VULKAN)
        case QRhi::Vulkan: {
            QRhiVulkanInitParams params;
            params.inst = setup_.vulkan.get();
            rhi_.reset(QRhi::create(QRhi::Vulkan, &params, flags));
            break;
        }
#endif
#if defined(BPS_RHI_OPENGL)
        case QRhi::OpenGLES2: {
            QRhiGles2InitParams params;
            params.fallbackSurface = setup_.glSurface.get();
            rhi_.reset(QRhi::create(QRhi::OpenGLES2, &params, flags));
            break;
        }
#endif
        default:
            break;
        }
        if (!rhi_) {
            if (why) *why = std::format("QRhi could not create a {} device", setup_.api);
            return false;
        }
        QRhiDriverInfo info = rhi_->driverInfo();
        driverName_ = info.deviceName.toStdString();

        // ---- the shaders (built by qsb for every API at compile time) ----
        auto loadShader = [](const char* path) {
            QFile f(QString::fromLatin1(path));
            if (!f.open(QIODevice::ReadOnly))
                return QShader();
            return QShader::fromSerialized(f.readAll());
        };
        const QShader vs = loadShader(":/bps/shaders/engine.vert.qsb");
        const QShader fs = loadShader(":/bps/shaders/engine.frag.qsb");
        if (!vs.isValid() || !fs.isValid()) {
            if (why) *why = "the compiled engine shaders are missing from the build";
            return false;
        }

        // ---- fixed resources ----
        const float corners[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        vbuf_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(corners)));
        if (!vbuf_->create()) { if (why) *why = "vertex buffer"; return false; }
        std::memcpy(cornerBytes_, corners, sizeof(corners));

        sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!sampler_->create()) { if (why) *why = "sampler"; return false; }

        // A 1x1 white texture stands in wherever a draw has no picture (the shader reads the slot only
        // when its mode needs it, but every binding must name a live texture).
        white_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1, {}));
        if (!white_->create()) { if (why) *why = "dummy texture"; return false; }
        whitePending_ = true;

        stride_ = static_cast<uint32_t>(rhi_->ubufAlignment());
        stride_ = (static_cast<uint32_t>(sizeof(DrawConstants)) + stride_ - 1u) / stride_ * stride_;
        // ONE fixed-size dynamic uniform buffer, never regrown: pipelines and bindings name it, and a frame
        // with more draws than it holds is simply executed in several chunks (see SubmitOnGpuThread).
        ubuf_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                    static_cast<quint32>(kMaxDraws * stride_)));
        if (!ubuf_->create()) { if (why) *why = "uniform buffer"; return false; }

        // ---- a render-pass descriptor every frame target shares (pipelines are built against it) ----
        {
            std::unique_ptr<QRhiTexture> probe(rhi_->newTexture(
                QRhiTexture::RGBA8, QSize(1, 1), 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
            if (!probe->create()) { if (why) *why = "probe target"; return false; }
            QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(probe.get())};
            std::unique_ptr<QRhiTextureRenderTarget> probeRt(
                rhi_->newTextureRenderTarget(desc, QRhiTextureRenderTarget::PreserveColorContents));
            rpd_.reset(probeRt->newCompatibleRenderPassDescriptor());
            probeRt->setRenderPassDescriptor(rpd_.get());
            if (!probeRt->create()) { if (why) *why = "probe render target"; return false; }
        }

        // ---- the four pipelines: the same shader under four blend states ----
        QRhiShaderResourceBindings* layoutSrb = SrbFor(white_.get(), white_.get());
        if (!layoutSrb) { if (why) *why = "bindings"; return false; }
        for (int p = 0; p < kPipeCount; ++p) {
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.colorWrite = QRhiGraphicsPipeline::ColorMask(0xF);
            switch (static_cast<Pipe>(p)) {
            case Pipe::Alpha:
                // The target is premultiplied: colour by source alpha, alpha straight over.
                blend.enable = true;
                blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
                blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
                blend.opColor = QRhiGraphicsPipeline::Add;
                blend.srcAlpha = QRhiGraphicsPipeline::One;
                blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
                blend.opAlpha = QRhiGraphicsPipeline::Add;
                break;
            case Pipe::Add:   // dst + src*a, alpha = max(da, a)
                blend.enable = true;
                blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
                blend.dstColor = QRhiGraphicsPipeline::One;
                blend.opColor = QRhiGraphicsPipeline::Add;
                blend.srcAlpha = QRhiGraphicsPipeline::SrcAlpha;
                blend.dstAlpha = QRhiGraphicsPipeline::One;
                blend.opAlpha = QRhiGraphicsPipeline::Max;
                break;
            case Pipe::Mul:   // the CPU's dst*(1 - a*(1-src)): the shader emits (1-src, a), REV_SUBTRACT does the rest
                blend.enable = true;
                blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
                blend.dstColor = QRhiGraphicsPipeline::One;
                blend.opColor = QRhiGraphicsPipeline::ReverseSubtract;
                blend.srcAlpha = QRhiGraphicsPipeline::SrcAlpha;
                blend.dstAlpha = QRhiGraphicsPipeline::One;
                blend.opAlpha = QRhiGraphicsPipeline::Max;
                break;
            case Pipe::Replace:   // effect / blend passes and clears write the final pixel
                blend.enable = false;
                break;
            }
            auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
            pipeline->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
            QRhiVertexInputLayout layout;
            layout.setBindings({QRhiVertexInputBinding(2 * sizeof(float))});
            layout.setAttributes({QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0)});
            pipeline->setVertexInputLayout(layout);
            pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            pipeline->setCullMode(QRhiGraphicsPipeline::None);   // a quad is drawn either way round
            pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
            pipeline->setTargetBlends({blend});
            pipeline->setShaderResourceBindings(layoutSrb);
            pipeline->setRenderPassDescriptor(rpd_.get());
            if (!pipeline->create()) {
                if (why) *why = std::format("the {} pipeline did not build", p);
                return false;
            }
            pipelines_[p] = std::move(pipeline);
        }

        const int maxTex = rhi_->resourceLimit(QRhi::TextureSizeMax);
        caps_ = BackendCapabilities{kRhiBackendName, true, maxTex > 0 ? maxTex : 8192, 0, true, true, true};
        yUpFramebuffer_ = rhi_->isYUpInFramebuffer();
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // Bindings: one per (slot-1 texture, slot-2 texture) pair, built on demand and dropped with either texture.
    QRhiShaderResourceBindings* SrbFor(QRhiTexture* t1, QRhiTexture* t2) {
        const auto key = std::make_pair(t1, t2);
        auto it = srbs_.find(key);
        if (it != srbs_.end())
            return it->second.get();
        std::unique_ptr<QRhiShaderResourceBindings> srb(rhi_->newShaderResourceBindings());
        srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                ubuf_.get(), static_cast<quint32>(sizeof(DrawConstants))),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t1, sampler_.get()),
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, t2, sampler_.get()),
        });
        if (!srb->create())
            return nullptr;
        return srbs_.emplace(key, std::move(srb)).first->second.get();
    }

    void DropBindingsFor(QRhiTexture* tex) {
        for (auto it = srbs_.begin(); it != srbs_.end();) {
            if (it->first.first == tex || it->first.second == tex) it = srbs_.erase(it);
            else ++it;
        }
    }

    bool MakeTexture(const RgbaImage& image, Texture& out) {
        const int maxTex = rhi_->resourceLimit(QRhi::TextureSizeMax);
        if (image.width <= 0 || image.height <= 0 || (maxTex > 0 && (image.width > maxTex || image.height > maxTex)))
            return false;
        out.tex.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(image.width, image.height), 1, {}));
        return out.tex->create();
    }

    // ---------------------------------------------------------------------------------------------
    bool EnsureFrame(int width, int height) {
        if (frameTex_ && frameWidth_ == width && frameHeight_ == height)
            return true;
        ReleaseFrame();
        frameWidth_ = width;
        frameHeight_ = height;
        const QSize size(width, height);

        frameTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1,
                                         QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!frameTex_->create())
            return false;
        // The scratch copies an effect or blend pass reads (the frame may not be sampled while it is the target).
        scratchA_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1, {}));
        scratchB_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1, {}));
        if (!scratchA_->create() || !scratchB_->create())
            return false;

        QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(frameTex_.get())};
        rt_.reset(rhi_->newTextureRenderTarget(desc, QRhiTextureRenderTarget::PreserveColorContents));
        rt_->setRenderPassDescriptor(rpd_.get());
        if (!rt_->create())
            return false;
        frameFresh_ = true;   // a new frame texture is undefined until the first pass writes it
        return true;
    }

    void ReleaseFrame() {
        if (scratchA_) DropBindingsFor(scratchA_.get());
        if (scratchB_) DropBindingsFor(scratchB_.get());
        rt_.reset();
        scratchA_.reset();
        scratchB_.reset();
        frameTex_.reset();
    }

    void ReleaseGpu() {
        ReleaseFrame();
        textures_.clear();
        textureBytes_ = 0;
        srbs_.clear();
        for (auto& p : pipelines_) p.reset();
        rpd_.reset();
        ubuf_.reset();
        vbuf_.reset();
        sampler_.reset();
        white_.reset();
        rhi_.reset();   // last: every resource above belongs to it
    }

    // ---------------------------------------------------------------------------------------------
    // Translating the engine's commands. Nothing here touches the GPU.
    // ---------------------------------------------------------------------------------------------
    IRect ClampScissor(const Rect& r) const {
        const int x0 = std::clamp(static_cast<int>(r.x), 0, frameWidth_);
        const int y0 = std::clamp(static_cast<int>(r.y), 0, frameHeight_);
        const int x1 = std::clamp(static_cast<int>(r.Right()), 0, frameWidth_);
        const int y1 = std::clamp(static_cast<int>(r.Bottom()), 0, frameHeight_);
        return IRect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    }

    DrawConstants BaseConstants() const {
        DrawConstants c{};
        const QMatrix4x4 corr = rhi_->clipSpaceCorrMatrix();
        std::memcpy(c.clip, corr.constData(), sizeof(c.clip));
        c.frame[0] = static_cast<float>(frameWidth_);
        c.frame[1] = static_cast<float>(frameHeight_);
        c.misc[0] = yUpFramebuffer_ ? 1.0f : 0.0f;
        return c;
    }

    void PushDraw(Plan& plan, int mode, const Rect& rect, const Rect& uv, const Color& color, float radius,
                  QRhiTexture* t1, QRhiTexture* t2, Pipe pipe, const IRect& scissor, float effect = 0.0f,
                  float param = 0.0f, float blendMode = 0.0f, float blendFlags = 0.0f, float dirX = 0.0f,
                  float dirY = 0.0f, float dirZ = 0.0f) const {
        if (scissor.w <= 0 || scissor.h <= 0)
            return;   // nothing inside the scissor: the draw cannot touch a pixel
        DrawConstants c = BaseConstants();
        c.rect[0] = rect.x; c.rect[1] = rect.y; c.rect[2] = rect.width; c.rect[3] = rect.height;
        c.uv[0] = uv.x; c.uv[1] = uv.y; c.uv[2] = uv.x + uv.width; c.uv[3] = uv.y + uv.height;
        c.color[0] = color.r; c.color[1] = color.g; c.color[2] = color.b; c.color[3] = color.a;
        c.params[0] = radius; c.params[1] = effect; c.params[2] = param; c.params[3] = static_cast<float>(mode);
        c.frame[2] = blendMode; c.frame[3] = blendFlags;
        c.dir[0] = dirX; c.dir[1] = dirY; c.dir[2] = dirZ;
        Op op;
        op.kind = Op::Kind::Draw;
        op.pipe = pipe;
        op.t1 = t1 ? t1 : white_.get();
        op.t2 = t2 ? t2 : white_.get();
        op.offset = static_cast<uint32_t>(plan.constants.size()) * stride_;
        op.scissor = scissor;
        plan.constants.push_back(c);
        plan.ops.push_back(op);
        ++plan.draws;
    }

    void PushCopy(Plan& plan, QRhiTexture* to) const {
        Op op;
        op.kind = Op::Kind::CopyFrame;
        op.copyTo = to;
        plan.ops.push_back(op);
    }

    // One quad = one command (the D3D11 backend's DrawQuad).
    void Quad(Plan& plan, int mode, const Rect& rect, const Rect& uv, const Color& color, float radius,
              QRhiTexture* tex, BlendMode blend) const {
        PushDraw(plan, mode, rect, uv, color, radius, tex, nullptr, PipeFor(blend), IRect{0, 0, frameWidth_, frameHeight_});
    }

    // The blend pass: composite the object against a copy of the frame under it (the D3D11 BlendQuad).
    void BlendQuad(Plan& plan, int shape, const Rect& rect, const Rect& uv, const Color& color, float radius,
                   QRhiTexture* tex, BlendMode blend) const {
        const IRect region = ClampScissor(rect);
        if (region.w <= 0 || region.h <= 0)
            return;
        PushCopy(plan, scratchB_.get());
        const float flags = static_cast<float>((tex ? 1 : 0) | (shape == kModeRounded ? 2 : 0));
        PushDraw(plan, kModeBlend, rect, uv, color, radius, tex, scratchB_.get(), Pipe::Replace, region, 0.0f, 0.0f,
                 static_cast<float>(static_cast<int>(blend)), flags);
    }

    void EffectPass(Plan& plan, int effect, float param, float radius, float dirX, float dirY, QRhiTexture* src,
                    const IRect& region) const {
        const Rect full(0, 0, static_cast<float>(frameWidth_), static_cast<float>(frameHeight_));
        PushDraw(plan, kModeEffect, full, Rect(0, 0, 1, 1), Color(1, 1, 1, 1), 0.0f, src, nullptr, Pipe::Replace, region,
                 static_cast<float>(effect), param, 0.0f, 0.0f, dirX, dirY, radius);
    }

    void ApplyEffect(Plan& plan, int effect, float param, const Rect& region) const {
        if (region.width <= 0.0f || region.height <= 0.0f)
            return;
        const bool blur = (effect == 0);
        // The ids the compositor implements; the others are composite operations upstream, which the CPU
        // rasterizer ignores too.
        if (!blur && effect != 3 && effect != 4 && effect != 5 && effect != 6)
            return;
        // The scissor limits the WRITE; a blur's taps still read outside the region, like the CPU filter
        // averaging neighbours beyond it.
        const IRect scissor = ClampScissor(region);
        if (blur) {
            // Separable box blur: horizontal from a copy, then vertical from a copy of that result.
            const float radius = std::clamp(param, 1.0f, 64.0f);
            PushCopy(plan, scratchA_.get());
            EffectPass(plan, 0, param, radius, 1.0f, 0.0f, scratchA_.get(), scissor);
            PushCopy(plan, scratchB_.get());
            EffectPass(plan, 0, param, radius, 0.0f, 1.0f, scratchB_.get(), scissor);
        } else {
            PushCopy(plan, scratchA_.get());
            EffectPass(plan, effect, param, 0.0f, 0.0f, 0.0f, scratchA_.get(), scissor);
        }
    }

    void Translate(const DrawCommand& cmd, Plan& plan) {
        const Rect full(0, 0, static_cast<float>(frameWidth_), static_cast<float>(frameHeight_));
        switch (cmd.type) {
        case DrawCommand::Type::Clear: {
            // A clear is a full-frame REPLACE of the premultiplied colour (no clear-flag render passes).
            const Color premul(cmd.color.r * cmd.color.a, cmd.color.g * cmd.color.a, cmd.color.b * cmd.color.a, cmd.color.a);
            PushDraw(plan, kModeSolid, full, Rect(0, 0, 0, 0), premul, 0.0f, nullptr, nullptr, Pipe::Replace,
                     IRect{0, 0, frameWidth_, frameHeight_});
            break;
        }
        case DrawCommand::Type::SetBlend:
            break;   // the blend travels on each command (cmd.blend)
        case DrawCommand::Type::FillRect: {
            // EDGE-SNAPPED (RenderTypes' SnapRectToPixelEdges): the CPU path's rule, so abutting fills
            // share one pixel boundary.
            const Rect r = SnapRectToPixelEdges(cmd.rect);
            const BlendMode blend = static_cast<BlendMode>(cmd.blend);
            if (NeedsBlendPass(blend)) {
                BlendQuad(plan, kModeSolid, r, Rect(0, 0, 0, 0), cmd.color, 0.0f, nullptr, blend);
                break;
            }
            Quad(plan, blend == BlendMode::Multiply ? kModeInvertAdd : kModeSolid, r, Rect(0, 0, 0, 0), cmd.color,
                 0.0f, nullptr, blend);
            break;
        }
        case DrawCommand::Type::BlitImage:
        case DrawCommand::Type::DrawGlyph: {
            auto it = textures_.find(cmd.texture.id);
            if (it == textures_.end() || !it->second.cpu) {
                static std::once_flag once;
                std::call_once(once, [] {
                    Logger::Instance().Warning("blit: a texture id this backend does not hold - nothing drawn", "Graphics");
                });
                break;
            }
            const RgbaImage& src = *it->second.cpu;
            // A glyph lives in a shared atlas and carries its own cell in srcRect; a blit without one
            // means the whole image.
            Rect uv = cmd.srcRect;
            if (uv.width <= 0.0f || uv.height <= 0.0f)
                uv = Rect(0, 0, static_cast<float>(src.width), static_cast<float>(src.height));
            uv.x /= static_cast<float>(src.width);
            uv.y /= static_cast<float>(src.height);
            uv.width /= static_cast<float>(src.width);
            uv.height /= static_cast<float>(src.height);
            const Rect dst(static_cast<float>(static_cast<int>(cmd.rect.x)), static_cast<float>(static_cast<int>(cmd.rect.y)),
                           static_cast<float>(static_cast<int>(cmd.rect.width)), static_cast<float>(static_cast<int>(cmd.rect.height)));
            const BlendMode blend = static_cast<BlendMode>(cmd.blend);
            if (NeedsBlendPass(blend)) {
                // The picture's mode (the MEDIA tab's Blend dropdown) is computed against the frame under it.
                BlendQuad(plan, kModeTextured, dst, uv, cmd.color, 0.0f, it->second.tex.get(), blend);
                break;
            }
            Quad(plan, kModeTextured + (blend == BlendMode::Multiply ? kModeInvertAdd : 0), dst, uv, cmd.color, 0.0f,
                 it->second.tex.get(), blend);
            break;
        }
        case DrawCommand::Type::DrawShape: {
            if (cmd.color.a <= 0.0f)
                break;
            const Rect r = SnapRectToPixelEdges(cmd.rect);
            const BlendMode blend = static_cast<BlendMode>(cmd.blend);
            const int shape = cmd.cornerRadius >= 0.5f ? kModeRounded : kModeSolid;
            if (NeedsBlendPass(blend)) {
                BlendQuad(plan, shape, r, Rect(0, 0, 0, 0), cmd.color, cmd.cornerRadius, nullptr, blend);
                break;
            }
            // MULTIPLY needs the inverted form for shapes too (the D3D11 backend only inverts fills).
            Quad(plan, shape + (blend == BlendMode::Multiply ? kModeInvertAdd : 0), r, Rect(0, 0, 0, 0), cmd.color,
                 cmd.cornerRadius, nullptr, blend);
            break;
        }
        case DrawCommand::Type::PushEffect:
            pending_.active = true;
            pending_.region = cmd.rect;
            pending_.effect = cmd.effect;
            pending_.param = cmd.effectParam;
            break;
        case DrawCommand::Type::PopEffect:
            if (pending_.active) {
                ApplyEffect(plan, pending_.effect, pending_.param, pending_.region);
                pending_ = PendingEffect{};
            }
            break;
        }
    }

    // ---------------------------------------------------------------------------------------------
    Result<void> SubmitOnGpuThread(const std::vector<DrawCommand>& commands) {
        if (!rhi_ || !rt_)
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "no render target");

        Plan plan;
        plan.ops.reserve(commands.size() + 8);
        plan.constants.reserve(commands.size() + 8);
        // A frame texture nothing has written yet is undefined: start it transparent (the D3D11 backend
        // clears at creation; a leading Clear command makes this a harmless extra full-frame write).
        if (frameFresh_) {
            DrawCommand clear;
            clear.type = DrawCommand::Type::Clear;
            clear.color = Color(0, 0, 0, 0);
            Translate(clear, plan);
            frameFresh_ = false;
        }
        for (const DrawCommand& cmd : commands)
            Translate(cmd, plan);
        if (plan.ops.empty())
            return Ok();

        // Pre-resolve every binding (creating one is not allowed mid-pass on every API).
        for (Op& op : plan.ops) {
            if (op.kind != Op::Kind::Draw) continue;
            if (!SrbFor(op.t1, op.t2))
                return Error::Make(Err::Render_BackendUnavailable, "Rhi", "binding creation failed");
        }

        // The uniform buffer holds kMaxDraws blocks. A frame with more draws than that (not seen in
        // practice: a lyric slide is hundreds) runs as several offscreen frames, each uploading its own
        // blocks — frames are synchronous, so the buffer is free again for the next chunk.
        size_t opBegin = 0;
        while (opBegin < plan.ops.size()) {
            size_t opEnd = opBegin;
            size_t draws = 0;
            uint32_t firstOffset = 0;
            while (opEnd < plan.ops.size()) {
                const Op& o = plan.ops[opEnd];
                if (o.kind == Op::Kind::Draw) {
                    if (draws == kMaxDraws) break;
                    if (draws == 0) firstOffset = o.offset;
                    ++draws;
                }
                ++opEnd;
            }
            const Result<void> r = RunChunk(plan, opBegin, opEnd, firstOffset, draws);
            if (!r.ok())
                return r;
            opBegin = opEnd;
        }
        return Ok();
    }

    // Records and runs ops [opBegin, opEnd) as one offscreen frame.
    Result<void> RunChunk(const Plan& plan, size_t opBegin, size_t opEnd, uint32_t firstOffset, size_t draws) {
        QRhiCommandBuffer* cb = nullptr;
        if (rhi_->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb)
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "beginOffscreenFrame failed");

        // ---- everything the frame needs on the GPU before its first pass ----
        QRhiResourceUpdateBatch* batch = rhi_->nextResourceUpdateBatch();
        if (!vbufUploaded_) {
            batch->uploadStaticBuffer(vbuf_.get(), 0, sizeof(cornerBytes_), cornerBytes_);
            vbufUploaded_ = true;
        }
        if (whitePending_) {
            static const uint8_t kWhite[4] = {255, 255, 255, 255};
            QRhiTextureSubresourceUploadDescription sub(reinterpret_cast<const char*>(kWhite), 4);
            sub.setSourceSize(QSize(1, 1));
            batch->uploadTexture(white_.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, sub)}));
            whitePending_ = false;
        }
        std::vector<SharedRgbaImage> inFlight;   // keeps the pixels alive until the frame has finished
        for (auto& [id, t] : textures_) {
            if (!t.pending || !t.tex) continue;
            // The upload REFERENCES the picture (no 8 MB copy per video frame); inFlight holds it.
            QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(t.pending->pixels.data()),
                                                       static_cast<qsizetype>(t.pending->pixels.size() * sizeof(uint32_t)));
            QRhiTextureSubresourceUploadDescription sub(bytes);
            sub.setSourceSize(QSize(t.pending->width, t.pending->height));
            batch->uploadTexture(t.tex.get(), QRhiTextureUploadDescription({QRhiTextureUploadEntry(0, 0, sub)}));
            inFlight.push_back(std::move(t.pending));
            t.pending.reset();
        }
        if (draws > 0) {
            const size_t firstIndex = firstOffset / stride_;
            std::vector<uint8_t> ubytes(draws * stride_, 0);
            for (size_t i = 0; i < draws; ++i)
                std::memcpy(ubytes.data() + i * stride_, &plan.constants[firstIndex + i], sizeof(DrawConstants));
            batch->updateDynamicBuffer(ubuf_.get(), 0, static_cast<quint32>(ubytes.size()), ubytes.data());
        }
        cb->resourceUpdate(batch);

        // ---- the passes ----
        const QRhiViewport viewport(0.0f, 0.0f, static_cast<float>(frameWidth_), static_cast<float>(frameHeight_));
        const QRhiCommandBuffer::VertexInput vertexInput(vbuf_.get(), 0);
        bool inPass = false;
        auto beginPass = [&] {
            cb->beginPass(rt_.get(), QColor(0, 0, 0, 0), QRhiDepthStencilClearValue(1.0f, 0));
            cb->setViewport(viewport);
            inPass = true;
        };
        for (size_t opIndex = opBegin; opIndex < opEnd; ++opIndex) {
            const Op& op = plan.ops[opIndex];
            if (op.kind == Op::Kind::CopyFrame) {
                // A texture cannot be copied while a pass is recording into it: close the pass first.
                if (inPass) { cb->endPass(); inPass = false; }
                QRhiResourceUpdateBatch* copy = rhi_->nextResourceUpdateBatch();
                QRhiTextureCopyDescription desc;
                desc.setPixelSize(QSize(frameWidth_, frameHeight_));
                copy->copyTexture(op.copyTo, frameTex_.get(), desc);
                cb->resourceUpdate(copy);
                continue;
            }
            if (!inPass) beginPass();
            cb->setGraphicsPipeline(pipelines_[static_cast<int>(op.pipe)].get());
            // QRhi scissors have a BOTTOM-left origin on every API.
            cb->setScissor(QRhiScissor(op.scissor.x, frameHeight_ - (op.scissor.y + op.scissor.h), op.scissor.w, op.scissor.h));
            const QRhiCommandBuffer::DynamicOffset dynamic(0, op.offset - firstOffset);
            cb->setShaderResources(SrbFor(op.t1, op.t2), 1, &dynamic);
            cb->setVertexInput(0, 1, &vertexInput);
            cb->draw(4);
        }
        if (inPass) cb->endPass();
        if (rhi_->endOffscreenFrame() != QRhi::FrameOpSuccess)
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "endOffscreenFrame failed");
        return Ok();
    }

    Result<RgbaImage> ReadbackOnGpuThread() {
        if (!rhi_ || !frameTex_)
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "no frame to read");
        if (frameFresh_) {
            // Nothing has been drawn since the frame texture was created: it is transparent by definition
            // (the texture itself is undefined until the first pass writes it).
            RgbaImage blank;
            blank.width = frameWidth_;
            blank.height = frameHeight_;
            blank.pixels.assign(static_cast<size_t>(frameWidth_) * static_cast<size_t>(frameHeight_), 0u);
            return blank;
        }
        QRhiCommandBuffer* cb = nullptr;
        if (rhi_->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess || !cb)
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "beginOffscreenFrame failed");
        QRhiReadbackResult result;
        QRhiResourceUpdateBatch* batch = rhi_->nextResourceUpdateBatch();
        batch->readBackTexture(QRhiReadbackDescription(frameTex_.get()), &result);
        cb->resourceUpdate(batch);
        if (rhi_->endOffscreenFrame() != QRhi::FrameOpSuccess)
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "readback frame failed");

        const size_t rowBytes = static_cast<size_t>(frameWidth_) * sizeof(uint32_t);
        if (result.data.size() < static_cast<qsizetype>(rowBytes * static_cast<size_t>(frameHeight_)))
            return Error::Make(Err::Render_BackendUnavailable, "Rhi", "the readback came back short");
        RgbaImage img;
        img.width = frameWidth_;
        img.height = frameHeight_;
        img.pixels.resize(static_cast<size_t>(frameWidth_) * static_cast<size_t>(frameHeight_));
        const auto* base = reinterpret_cast<const uint8_t*>(result.data.constData());
        for (int y = 0; y < frameHeight_; ++y) {
            // OpenGL stores a render target bottom-up: the first row read is the picture's last.
            const int srcRow = yUpFramebuffer_ ? (frameHeight_ - 1 - y) : y;
            std::memcpy(reinterpret_cast<uint8_t*>(img.pixels.data()) + static_cast<size_t>(y) * rowBytes,
                        base + static_cast<size_t>(srcRow) * rowBytes, rowBytes);
        }
        for (uint32_t& px : img.pixels) {
            const uint32_t a = px >> 24;
            if (a != 0u && a != 255u)
                px = Unpremultiply(px);
        }
        return img;
    }

    // ---------------------------------------------------------------------------------------------
    static constexpr size_t kMaxDraws = 8192;   // x stride (256 B) = a 2 MB uniform buffer

    BackendCapabilities caps_{kRhiBackendName, true, 8192, 0, true, true, true};
    RhiSetup setup_;
    std::unique_ptr<GpuExecutor> executor_;

    // Everything below is touched ONLY on the GPU thread.
    std::unique_ptr<QRhi> rhi_;
    std::string driverName_;
    bool yUpFramebuffer_ = false;

    std::unique_ptr<QRhiBuffer> vbuf_;
    std::unique_ptr<QRhiBuffer> ubuf_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiTexture> white_;
    std::unique_ptr<QRhiRenderPassDescriptor> rpd_;
    std::unique_ptr<QRhiGraphicsPipeline> pipelines_[kPipeCount];
    std::map<std::pair<QRhiTexture*, QRhiTexture*>, std::unique_ptr<QRhiShaderResourceBindings>> srbs_;
    float cornerBytes_[8] = {};
    bool vbufUploaded_ = false;
    bool whitePending_ = false;
    uint32_t stride_ = 256;

    std::unique_ptr<QRhiTexture> frameTex_;
    std::unique_ptr<QRhiTexture> scratchA_;
    std::unique_ptr<QRhiTexture> scratchB_;
    std::unique_ptr<QRhiTextureRenderTarget> rt_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    bool frameFresh_ = false;
    PendingEffect pending_;

    std::unordered_map<uint64_t, Texture> textures_;
    uint64_t nextTexture_ = 1;
    std::atomic<uint64_t> textureBytes_{0};

    mutable std::mutex shaderMutex_;
    std::map<uint32_t, std::string> shaders_;
    uint32_t nextShader_ = 1;
};

} // namespace

// ===========================================================================================
// Factory: pick the best API this machine has and bring the backend up on it.
// ===========================================================================================
std::unique_ptr<IGraphicsBackend> CreateRhiBackend(std::string* failureReason) {
    auto fail = [failureReason](std::string why) -> std::unique_ptr<IGraphicsBackend> {
        if (failureReason) *failureReason = std::move(why);
        return nullptr;
    };

    struct Candidate { QRhi::Implementation impl; const char* name; };
    std::vector<Candidate> order;
    const bool gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
#if defined(Q_OS_WIN)
    order.push_back({QRhi::D3D11, "Direct3D 11"});
#elif defined(Q_OS_MACOS)
    order.push_back({QRhi::Metal, "Metal"});
#else
    // OpenGL first on Linux (every desktop driver stack has it), Vulkan as the second choice.
#  if defined(BPS_RHI_OPENGL)
    if (gui) order.push_back({QRhi::OpenGLES2, "OpenGL"});
#  endif
#  if defined(BPS_RHI_VULKAN)
    if (gui) order.push_back({QRhi::Vulkan, "Vulkan"});
#  endif
#endif

    // BPS_RHI_API=d3d11|metal|vulkan|gl picks one API (to test an OS's path on a machine that has several).
    const QByteArray forced = qgetenv("BPS_RHI_API").toLower();
    if (!forced.isEmpty()) {
        std::vector<Candidate> only;
        auto add = [&](QRhi::Implementation impl, const char* name) { only.push_back({impl, name}); };
        if (forced == "d3d11") add(QRhi::D3D11, "Direct3D 11");
        else if (forced == "metal") add(QRhi::Metal, "Metal");
        else if (forced == "vulkan") add(QRhi::Vulkan, "Vulkan");
        else if (forced == "gl" || forced == "opengl") add(QRhi::OpenGLES2, "OpenGL");
        order = std::move(only);
    }
    if (order.empty())
        return fail(gui ? "no GPU API Qt can use on this OS" : "no Qt GUI application is running to host a GPU device");

    std::string reasons;
    for (const Candidate& c : order) {
        RhiSetup setup;
        setup.impl = c.impl;
        setup.api = c.name;
#if defined(BPS_RHI_OPENGL)
        if (c.impl == QRhi::OpenGLES2) {
            if (!gui) continue;
            // QOffscreenSurface is created on the thread the engine starts on (the GUI thread).
            setup.glSurface.reset(NewGlSurfaceOnMainThread());
            if (!setup.glSurface) { reasons += "OpenGL: no offscreen surface; "; continue; }
        }
#endif
#if defined(BPS_RHI_VULKAN)
        if (c.impl == QRhi::Vulkan) {
            if (!gui) continue;
            setup.vulkan = std::make_unique<QVulkanInstance>();
            setup.vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
            if (!setup.vulkan->create()) { reasons += "Vulkan: no instance; "; continue; }
        }
#endif
        auto backend = std::make_unique<RhiGraphicsBackend>(std::move(setup));
        std::string why;
        if (backend->Init(&why)) {
            Logger::Instance().Info(
                std::format("QRhi backend ready on {} ({}; frames come back to the CPU through a readback)", c.name,
                            backend->Driver().empty() ? std::string("unnamed device") : backend->Driver()),
                kModule);
            return backend;
        }
        reasons += std::format("{}: {}; ", c.name, why);
    }
    return fail(reasons.empty() ? "no GPU API could be started" : reasons);
}

} // namespace bps::rendering

#else   // !BPS_HAVE_RHI

namespace bps::rendering {
std::unique_ptr<IGraphicsBackend> CreateRhiBackend(std::string* failureReason) {
    if (failureReason)
        *failureReason = "this build has no QRhi backend (Qt GuiPrivate / Shader Tools missing)";
    return nullptr;
}
} // namespace bps::rendering

#endif
