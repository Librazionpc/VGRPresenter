#pragma once

// GPU resource management (docs/specs/17 §GPU Resource Manager / §Render Cache
// / §Resource Uploader / §Frame Graph). The manager owns texture/buffer/shader/
// mesh lifetimes with reuse + eviction; the cache caches glyphs/layouts/
// geometry; the uploader loads async without blocking rendering; the frame
// graph tracks per-frame resource usage.

#include "core/common/Common.hpp"
#include "modules/rendering/IGraphicsBackend.hpp"
#include "modules/rendering/TextEngine.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {

class GPUResourceManager {
public:
    GPUResourceManager() = default;
    void BindBackend(IGraphicsBackend* backend) { backend_ = backend; }

    // Texture registry (dedup by name; reuse when unchanged).
    Result<TextureId> AcquireTexture(std::string_view name, const RgbaImage& image);
    Result<void> ReleaseTexture(std::string_view name);
    Result<TextureId> GetTexture(std::string_view name) const;
    size_t TextureCount() const;
    uint64_t TextureMemoryBytes() const { return backend_ ? backend_->TextureMemoryBytes() : 0; }

    // Shader registry.
    Result<uint32_t> AcquireShader(std::string_view name, std::string_view source);
    size_t ShaderCount() const { return backend_ ? backend_->ShaderCount() : 0; }

    // Eviction under memory pressure: release textures with refcount 0.
    size_t EvictIdle(uint64_t keepBytes);
    void Clear();

    // Capacity policy (from the Adaptive Runtime).
    void SetTextureBudget(uint64_t bytes) { textureBudget_ = bytes; }
    uint64_t TextureBudget() const { return textureBudget_; }

private:
    struct Entry {
        TextureId id;
        std::string name;
        int refs = 1;
        uint64_t bytes = 0;
    };
    mutable std::mutex mutex_;
    IGraphicsBackend* backend_ = nullptr;
    std::map<std::string, Entry, std::less<>> textures_;
    uint64_t textureBudget_ = 0;   // 0 = unlimited
};

// --- RenderCache: glyphs / text layouts / geometry --------------------------
class RenderCache {
public:
    RenderCache() = default;

    // Glyph atlas cache (per font size).
    struct AtlasEntry {
        RgbaImage atlas;
        std::map<uint8_t, FontGlyph> glyphs;
        TextureId texture;
    };
    Result<const AtlasEntry*> GlyphAtlas(float sizePx);
    // Font-identity-aware form: builds via FontManager::BuildSystemAtlas
    // when style.fontId names a real, resolvable font, else falls back to
    // the plain builtin atlas above (same cache, same key space — a style
    // whose font isn't available reads exactly as "builtin" always has).
    // Kept as a SEPARATE overload rather than changing GlyphAtlas(float)'s
    // own signature: every existing caller (including the unit tests) that
    // only ever wanted the builtin font keeps working unchanged.
    Result<const AtlasEntry*> GlyphAtlas(const TextStyle& style, const FontManager& fonts);
    void InvalidateGlyphAtlas();

    // Text layout cache (per text + style).
    Result<TextLayoutResult> Layout(const std::string& text, const TextStyle& style,
                                    const FontManager& fonts);
    void InvalidateLayouts();

    size_t LayoutCount() const;
    size_t AtlasCount() const;
    void Clear();

private:
    mutable std::mutex mutex_;
    std::map<int, std::unique_ptr<AtlasEntry>> atlases_;   // keyed by px size
    // Font-identity-aware atlases (GlyphAtlas(style, fonts)) — a SEPARATE
    // map from atlases_ above: that one is always the builtin font, keyed
    // by size alone, and every pre-existing caller keeps using it unchanged.
    struct FontAtlasKey {
        std::string fontId;
        int sizePx = 0;
        bool bold = false;
        bool italic = false;
        bool operator<(const FontAtlasKey& o) const {
            if (fontId != o.fontId) return fontId < o.fontId;
            if (sizePx != o.sizePx) return sizePx < o.sizePx;
            if (bold != o.bold) return bold < o.bold;
            return italic < o.italic;
        }
    };
    std::map<FontAtlasKey, std::unique_ptr<AtlasEntry>> fontAtlases_;
    struct LayoutKey {
        std::string text;
        std::string font;
        float size = 0;
        float wrap = 0;
        bool wrapOn = false;
        float letterSpacing = 0;
        // Layout-affecting and therefore key-relevant: lineSpacing changes the
        // line height (totalHeight), bold/italic change the real-font advances
        // (SystemCharAdvances takes them) and with them every wrap decision.
        // Omitting any of these served a stale layout whenever the SAME text
        // was laid out twice under styles differing only here — measured
        // fitting, drawn overflowing.
        float lineSpacing = 0;
        bool bold = false;
        bool italic = false;
        bool operator<(const LayoutKey& o) const {
            if (text != o.text) return text < o.text;
            if (font != o.font) return font < o.font;
            if (size != o.size) return size < o.size;
            if (wrap != o.wrap) return wrap < o.wrap;
            if (wrapOn != o.wrapOn) return wrapOn < o.wrapOn;
            if (letterSpacing != o.letterSpacing) return letterSpacing < o.letterSpacing;
            if (lineSpacing != o.lineSpacing) return lineSpacing < o.lineSpacing;
            if (bold != o.bold) return bold < o.bold;
            return italic < o.italic;
        }
    };
    std::map<LayoutKey, TextLayoutResult> layouts_;
    FontManager fonts_;
};

// --- ResourceUploader: async uploads via the core ThreadPool ----------------
class ResourceUploader {
public:
    // Queue an async decode+upload. `decoder` produces the RGBA image from raw
    // bytes; `done` is called on completion (may be null). Decoding runs on the
    // core ThreadPool (never the render thread); Pump() performs the actual GPU
    // upload and fires `done`, so the render loop never blocks on decoding.
    using Decoder = std::function<RgbaImage(const std::vector<uint8_t>&)>;
    using DoneFn = std::function<void(Result<TextureId>)>;
    void Submit(std::string name, std::vector<uint8_t> bytes, Decoder decoder,
                DoneFn done);
    // Drains completed uploads: acquires the texture via the bound GPU manager
    // and fires the per-job done callback (called each frame by the renderer).
    void Pump();
    size_t Pending() const;
    void Shutdown();

    // The uploader needs the GPU manager to materialize decoded images into
    // textures; bind once at engine Initialize (and on backend switch).
    void BindGpu(GPUResourceManager* gpu) { gpu_ = gpu; }

private:
    struct Job {
        std::string name;
        std::vector<uint8_t> bytes;
        Decoder decoder;
        DoneFn done;
    };
    struct Decoded {
        std::string name;
        RgbaImage image;
        DoneFn done;
    };
    mutable std::mutex mutex_;
    std::vector<Job> queued_;
    std::vector<Decoded> decoded_;
    GPUResourceManager* gpu_ = nullptr;
};

// --- FrameGraph: per-frame resource tracking --------------------------------
struct FrameResource {
    std::string name;
    uint64_t bytes = 0;
    bool usedThisFrame = false;
};

class FrameGraph {
public:
    void BeginFrame();
    void Use(std::string_view name, uint64_t bytes);
    void EndFrame();
    // Resources not used for N frames are candidates for eviction.
    std::vector<std::string> IdleResources(int idleFrames) const;
    size_t LiveCount() const;

private:
    struct Entry {
        uint64_t bytes = 0;
        int unusedFrames = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> resources_;
    int frame_ = 0;
};

} // namespace bps::rendering
