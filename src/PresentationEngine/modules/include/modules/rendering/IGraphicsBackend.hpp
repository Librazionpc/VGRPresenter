#pragma once

#include "core/common/Common.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {
struct TextureId {
    uint64_t id = 0;
    bool valid() const { return id != 0; }
};

// A CPU-side RGBA8 image (decode/upload input and frame capture output).
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;   // Color::Pack format

    bool empty() const { return pixels.empty(); }
    size_t ByteSize() const { return pixels.size() * sizeof(uint32_t); }
};

// An immutable picture many holders can reference at once: one decoded video frame is held by the
// compositor, every output's scene object and every backend texture showing it as ONE allocation
// instead of a private 8 MB copy each (the copies were most of a software-path frame's cost).
using SharedRgbaImage = std::shared_ptr<const RgbaImage>;

// Spare frame-sized pixel buffers. A composed frame is 8 MB at 1080p, handed to the outputs and dropped
// when the next one replaces it; allocating a fresh one every frame meant ~6 ms of page faults and
// zeroing per render. A frame's deleter gives its buffer back here and the next BeginFrame takes it,
// so the steady state allocates nothing. A taken buffer is NOT cleared (the backend clears only what
// the frame does not overwrite anyway).
class FramebufferPool {
public:
    static FramebufferPool& Instance() {
        static FramebufferPool* pool = new FramebufferPool();   // leaked on purpose: frames outlive statics
        return *pool;
    }
    // A spare buffer of exactly `pixels` elements, or an empty vector.
    std::vector<uint32_t> Take(size_t pixels) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < spare_.size(); ++i)
            if (spare_[i].size() == pixels) {
                std::vector<uint32_t> out = std::move(spare_[i]);
                spare_.erase(spare_.begin() + static_cast<std::ptrdiff_t>(i));
                return out;
            }
        return {};
    }
    void Give(std::vector<uint32_t>&& buffer) {
        if (buffer.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        constexpr size_t kMaxSpare = 6;
        if (spare_.size() >= kMaxSpare) spare_.erase(spare_.begin());   // the oldest goes (frees its memory)
        spare_.push_back(std::move(buffer));
    }
private:
    std::mutex mutex_;
    std::vector<std::vector<uint32_t>> spare_;
};

// Capabilities a backend advertises (the renderer + AdaptiveRuntime query this
// instead of hardcoding API names).
struct BackendCapabilities {
    std::string name;             // "null" / "software" / "directx12" / ...
    bool gpuAccelerated = false;
    int maxTextureSize = 0;       // 0 = unlimited
    uint64_t maxTextureMemory = 0;
    bool supportsShaders = false; // software backend fakes shaders via pixel ops
    bool supportsOffscreen = true;
    bool supportsPresent = true;
};

struct DrawCommand {
    enum class Type : int {
        Clear = 0,
        FillRect,
        BlitImage,      // texture → dest rect
        DrawGlyph,      // atlas glyph → dest rect (from a texture)
        DrawShape,      // filled shape (rect/circle/polygon)
        PushEffect,     // begin pixel-effect region (brightness etc.)
        PopEffect,
        SetBlend,
    };
    Type type = Type::Clear;
    Rect rect;
    Color color;
    TextureId texture;
    Rect srcRect;               // source region (blit)
    float cornerRadius = 0.0f;
    uint32_t shaderId = 0;      // 0 = default
    int effect = 0;             // EffectType
    float effectParam = 0.0f;
    int blend = 0;              // BlendMode
};

class IGraphicsBackend {
public:
    virtual ~IGraphicsBackend() = default;

    // --- Capabilities ---
    virtual const BackendCapabilities& Capabilities() const noexcept = 0;

    // --- Texture management ---
    virtual Result<TextureId> CreateTexture(const RgbaImage& image) = 0;
    virtual Result<void> UpdateTexture(TextureId id, const RgbaImage& image) = 0;
    // The same for a picture held by pointer: a backend that keeps its textures in system memory
    // holds the pointer instead of copying the pixels. Default: the copying call.
    virtual Result<TextureId> CreateTextureShared(const SharedRgbaImage& image) { return CreateTexture(*image); }
    virtual Result<void> UpdateTextureShared(TextureId id, const SharedRgbaImage& image) { return UpdateTexture(id, *image); }
    virtual Result<void> DestroyTexture(TextureId id) = 0;
    virtual RgbaImage ReadTexture(TextureId id) const = 0;   // for capture/thumbnails
    virtual uint64_t TextureMemoryBytes() const noexcept = 0;

    // --- Shaders (recorded + validated; compiled where supported) ---
    virtual Result<uint32_t> CreateShader(std::string_view name,
                                          std::string_view source) = 0;
    virtual Result<void> DestroyShader(uint32_t id) = 0;
    virtual size_t ShaderCount() const noexcept = 0;

    // --- Frame lifecycle ---
    virtual Result<void> BeginFrame(int width, int height) = 0;
    virtual Result<void> Submit(const std::vector<DrawCommand>& commands) = 0;
    // Capture the last submitted frame as RGBA8 (offscreen/software backends;
    // GPU backends resolve via readback).
    virtual Result<RgbaImage> Readback() const = 0;
    // The same, when the caller takes OWNERSHIP of the frame (the next BeginFrame starts a new one):
    // a backend that keeps its framebuffer in system memory hands it over instead of copying 8 MB.
    // Default: the copying readback.
    virtual Result<RgbaImage> ReadbackMove() { return Readback(); }
    virtual Result<void> Present() = 0;
    virtual Result<void> EndFrame() = 0;
    virtual Result<void> Shutdown() = 0;
};
} // namespace bps::rendering