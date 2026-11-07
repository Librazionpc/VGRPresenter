#pragma once

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
// ---------------------------------------------------------------------------
class SoftwareGraphicsBackend final : public IGraphicsBackend {
public:
    const BackendCapabilities& Capabilities() const noexcept override {
        return caps_;
    }
    Result<TextureId> CreateTexture(const RgbaImage& image) override;
    Result<void> UpdateTexture(TextureId id, const RgbaImage& image) override;
    Result<TextureId> CreateTextureShared(const SharedRgbaImage& image) override;
    Result<void> UpdateTextureShared(TextureId id, const SharedRgbaImage& image) override;
    Result<void> DestroyTexture(TextureId id) override;
    RgbaImage ReadTexture(TextureId id) const override;
    uint64_t TextureMemoryBytes() const noexcept override { return textureBytes_; }

    Result<uint32_t> CreateShader(std::string_view name, std::string_view src) override;
    Result<void> DestroyShader(uint32_t id) override;
    size_t ShaderCount() const noexcept override { return shaders_.size(); }

    Result<void> BeginFrame(int width, int height) override;
    Result<void> Submit(const std::vector<DrawCommand>& commands) override;
    Result<RgbaImage> Readback() const override;
    Result<RgbaImage> ReadbackMove() override;
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
    void FillRoundedRectBlended(int x, int y, int w, int h, float radius, Color c,
                                BlendMode mode);
    void Blit(int x, int y, int w, int h, const RgbaImage& src, const Rect& srcRect,
              Color tint, BlendMode mode);
    void RasterizeShape(const DrawCommand& cmd);

    // ---- WHAT A NEW VIDEO FRAME DOES NOT REDRAW ----------------------------------------------
    // A live frame changes one texture (the video) and nothing else, yet the whole command list used
    // to be rasterised again: the clear, the scaled background, every fill and glyph UNDER the video.
    // The commands are an exact description of the pixels, so the leading run of commands that touch
    // no texture changed since the last Submit is rasterised once and kept as a snapshot keyed by a
    // hash of those commands; the next frame restores it (one memcpy) and draws only what follows.
    struct PrefixEntry {
        uint64_t hash = 0;
        size_t count = 0;
        int width = 0;
        int height = 0;
        uint64_t used = 0;
        std::vector<uint32_t> pixels;
    };
    std::vector<PrefixEntry> prefixCache_;
    uint64_t prefixTick_ = 0;
    std::unordered_map<uint64_t, uint64_t> textureVersion_;   // bumped by Create/UpdateTexture
    std::unordered_map<uint64_t, uint64_t> textureSeen_;      // the version the last Submit drew
    std::unordered_map<uint64_t, uint64_t> textureHash_;      // hash of the pixels (content identity)
    size_t StaticPrefixLength(const std::vector<DrawCommand>& commands) const;
    uint64_t HashPrefix(const std::vector<DrawCommand>& commands, size_t count) const;
    uint64_t HashCommands(const std::vector<DrawCommand>& commands, size_t from, size_t to) const;

    // ---- WHAT STAYS THE SAME ABOVE A MOVING VIDEO ---------------------------------------------------
    // Text, plates and logos are drawn AFTER the video, so a new video frame redraws every one of them
    // (a lyric slide: dozens of glyph tiles, ~10 ms). The commands after the last moving picture are
    // rasterised once onto a TRANSPARENT layer, kept with the rows/runs that actually hold ink, and each
    // frame blends only those runs over the video. Used once the same run has been seen twice in a row
    // (a run that changes every frame is just drawn).
    struct LayerRun { int y = 0, x0 = 0, x1 = 0; };
    struct OverlayEntry {
        uint64_t hash = 0;
        int width = 0;
        int height = 0;
        uint64_t used = 0;
        std::vector<uint32_t> pixels;
        std::vector<LayerRun> runs;
    };
    std::vector<OverlayEntry> overlayCache_;
    std::vector<uint64_t> overlaySeen_;
    uint64_t overlayTick_ = 0;

    // A scaled copy of a texture region, kept while the same picture is asked for at the same size:
    // a static background is scaled ONCE, and one video frame reaches every output's scene (each scene
    // holds its own copy of the pixels, equal in content) scaled once instead of once per scene.
    struct ScaledEntry {
        uint64_t hash = 0;
        int dw = 0, dh = 0;
        float sx = 0, sy = 0, sw = 0, sh = 0;
        bool opaque = false;
        uint64_t used = 0;
        RgbaImage image;
    };
    std::vector<ScaledEntry> scaledCache_;
    uint64_t scaledTick_ = 0;
    // Resamples \`src\`'s \`srcRect\` to w x h (the premultiplied separable filter), rows split over threads.
    static RgbaImage ResampleToImage(const RgbaImage& src, const Rect& srcRect, int w, int h,
                                     bool& opaqueOut);

    BackendCapabilities caps_{"software", false, 8192, 0, true, true, true};
    mutable std::mutex mutex_;
    // Textures are held by pointer: a picture that arrives shared (one video frame in every output's
    // scene) is NOT copied per texture, and a hash of it is computed once per picture (hashMemo_).
    std::unordered_map<uint64_t, SharedRgbaImage> textures_;
    struct HashMemoEntry { SharedRgbaImage image; uint64_t hash = 0; bool opaque = false; };
    std::vector<HashMemoEntry> hashMemo_;   // the last few pictures hashed (held, so a pointer match is safe)
    uint64_t HashOf(const SharedRgbaImage& image, bool* opaqueOut = nullptr);
    std::unordered_map<uint64_t, bool> textureOpaque_;   // every pixel alpha 255 (known per texture)
    uint64_t nextTexture_ = 1;
    uint64_t textureBytes_ = 0;
    std::map<uint32_t, std::string> shaders_;
    uint32_t nextShader_ = 1;

    RgbaImage fb_;        // active framebuffer
    bool fbNeedsClear_ = false;   // fb_ came from the buffer pool and still holds an old frame
    void ClearIfStale() {
        if (fbNeedsClear_) {
            std::fill(fb_.pixels.begin(), fb_.pixels.end(), 0u);
            fbNeedsClear_ = false;
        }
    }
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