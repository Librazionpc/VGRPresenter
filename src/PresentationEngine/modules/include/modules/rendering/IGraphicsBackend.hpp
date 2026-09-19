#pragma once

// IGraphicsBackend (docs/specs/17 §Backends): the ONLY graphics seam in the
// engine. The renderer talks exclusively to this interface; DirectX12 / Vulkan /
// Metal / OpenGL implementations are drop-in. Nothing outside this file touches
// a graphics API. Raw CPU-side pixels are exchanged as RGBA8 buffers so the
// interface stays backend-agnostic (a GPU backend uploads them to textures).

#include "core/common/Common.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
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

// A generic GPU command: the backend interprets high-level draw requests.
// Shader names are recorded (and compiled/validated where supported) but the
// software backend performs the equivalent pixel operations.
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
    virtual Result<void> Present() = 0;
    virtual Result<void> EndFrame() = 0;
    virtual Result<void> Shutdown() = 0;
};

} // namespace bps::rendering
