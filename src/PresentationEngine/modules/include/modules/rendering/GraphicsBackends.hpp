#pragma once

// Backend implementations (docs/specs/17 §Backends):
//  - NullGraphicsBackend: headless command accounting (CI, servers, tests).
//  - SoftwareGraphicsBackend: complete CPU rasterizer producing RGBA frames.
// DirectX12/Vulkan/Metal implement the same interface in future backends.

#include "modules/rendering/IGraphicsBackend.hpp"

#include <map>
#include <mutex>
#include <unordered_map>

namespace bps::rendering {

// ---------------------------------------------------------------------------
// NullGraphicsBackend — headless. Tracks counts + memory, produces a
// black/transparent frame on Readback (useful for previews without pixels).
// ---------------------------------------------------------------------------
class NullGraphicsBackend final : public IGraphicsBackend {
public:
    const BackendCapabilities& Capabilities() const noexcept override {
        return caps_;
    }
    Result<TextureId> CreateTexture(const RgbaImage& image) override;
    Result<void> UpdateTexture(TextureId id, const RgbaImage& image) override;
    Result<void> DestroyTexture(TextureId id) override;
    RgbaImage ReadTexture(TextureId id) const override;
    uint64_t TextureMemoryBytes() const noexcept override { return textureBytes_; }

    Result<uint32_t> CreateShader(std::string_view name, std::string_view src) override;
    Result<void> DestroyShader(uint32_t id) override;
    size_t ShaderCount() const noexcept override { return shaders_.size(); }

    Result<void> BeginFrame(int width, int height) override;
    Result<void> Submit(const std::vector<DrawCommand>& commands) override;
    Result<RgbaImage> Readback() const override;
    Result<void> Present() override { return Ok(); }
    Result<void> EndFrame() override { return Ok(); }
    Result<void> Shutdown() override;

    // Test hooks.
    uint64_t SubmitCount() const { return submitCount_; }
    size_t LiveTextures() const { return textures_.size(); }

private:
    BackendCapabilities caps_{"null", false, 0, 0, true, true, false};
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, RgbaImage> textures_;
    uint64_t nextTexture_ = 1;
    uint64_t textureBytes_ = 0;
    std::map<uint32_t, std::string> shaders_;
    uint32_t nextShader_ = 1;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    uint64_t submitCount_ = 0;
};

// ---------------------------------------------------------------------------
// SoftwareGraphicsBackend — CPU rasterizer.
// Supports clear, fill rect, image blits, glyph blits, filled shapes, and
// per-region pixel effects (brightness/contrast/saturation/opacity, box blur,
// glow, shadow, crop, mask). Frames are captured via Readback().
// ---------------------------------------------------------------------------
class SoftwareGraphicsBackend final : public IGraphicsBackend {
public:
    const BackendCapabilities& Capabilities() const noexcept override {
        return caps_;
    }
    Result<TextureId> CreateTexture(const RgbaImage& image) override;
    Result<void> UpdateTexture(TextureId id, const RgbaImage& image) override;
    Result<void> DestroyTexture(TextureId id) override;
    RgbaImage ReadTexture(TextureId id) const override;
    uint64_t TextureMemoryBytes() const noexcept override { return textureBytes_; }

    Result<uint32_t> CreateShader(std::string_view name, std::string_view src) override;
    Result<void> DestroyShader(uint32_t id) override;
    size_t ShaderCount() const noexcept override { return shaders_.size(); }

    Result<void> BeginFrame(int width, int height) override;
    Result<void> Submit(const std::vector<DrawCommand>& commands) override;
    Result<RgbaImage> Readback() const override;
    Result<void> Present() override { return Ok(); }
    Result<void> EndFrame() override { return Ok(); }
    Result<void> Shutdown() override;

    // --- Pixel helpers exposed for unit tests ---
    static uint32_t BlendPixel(uint32_t dst, uint32_t src, BlendMode mode);
    // Applies a pixel effect to a region of an image in place.
    static void ApplyEffect(RgbaImage& img, const Rect& region, int effect,
                            float param);

private:
    void Execute(const DrawCommand& cmd);
    void FillRectBlended(int x, int y, int w, int h, Color c, BlendMode mode);
    void Blit(int x, int y, int w, int h, const RgbaImage& src, const Rect& srcRect,
              Color tint, BlendMode mode);
    void RasterizeShape(const DrawCommand& cmd);

    BackendCapabilities caps_{"software", false, 8192, 0, true, true, true};
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, RgbaImage> textures_;
    uint64_t nextTexture_ = 1;
    uint64_t textureBytes_ = 0;
    std::map<uint32_t, std::string> shaders_;
    uint32_t nextShader_ = 1;

    RgbaImage fb_;        // active framebuffer
    int fbWidth_ = 0;
    int fbHeight_ = 0;

    // Effect region stack (PushEffect..PopEffect applies on Pop).
    struct PendingEffect {
        Rect region;
        int effect = 0;
        float param = 0.0f;
        bool active = false;
    };
    PendingEffect pending_;
};

} // namespace bps::rendering
