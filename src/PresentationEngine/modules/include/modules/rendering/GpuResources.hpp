#pragma once

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
    void BindBackend(IGraphicsBackend* backend);

    // Texture registry (dedup by name; reuse when unchanged).
    Result<TextureId> AcquireTexture(std::string_view name, const RgbaImage& image);
    Result<TextureId> UpdateTexture(std::string_view name, const RgbaImage& image,
                                    uint64_t rev);
    // The same for a picture held by pointer — handed to the backend without copying its pixels.
    Result<TextureId> UpdateTextureShared(std::string_view name, const SharedRgbaImage& image,
                                          uint64_t rev);
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
        uint64_t rev = 0;   // source frame version currently bound (UpdateTexture)
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
    struct FontAtlasKey {
        std::string fontId;
        int sizePx = 0;
        QFont::Weight fontWeight = QFont::Medium;
        bool italic = false;
        bool underline = false;
        bool strikeout = false;
        bool operator<(const FontAtlasKey& o) const {
            if (fontId != o.fontId) return fontId < o.fontId;
            if (sizePx != o.sizePx) return sizePx < o.sizePx;
            if (fontWeight != o.fontWeight) return fontWeight < o.fontWeight;
            if (italic != o.italic) return italic < o.italic;
            if (underline != o.underline) return underline < o.underline;
            return strikeout < o.strikeout;
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
        // Same reasoning as letterSpacing: it changes every measured width and
        // therefore every wrap decision.
        float wordSpacing = 0;
        float lineSpacing = 0;
        float lineGap = 0;
        bool bold = false;
        bool italic = false;
        QFont::Weight fontWeight = QFont::Medium;
        bool operator<(const LayoutKey& o) const {
            if (text != o.text) return text < o.text;
            if (font != o.font) return font < o.font;
            if (size != o.size) return size < o.size;
            if (wrap != o.wrap) return wrap < o.wrap;
            if (wrapOn != o.wrapOn) return wrapOn < o.wrapOn;
            if (letterSpacing != o.letterSpacing) return letterSpacing < o.letterSpacing;
            if (wordSpacing != o.wordSpacing) return wordSpacing < o.wordSpacing;
            if (lineSpacing != o.lineSpacing) return lineSpacing < o.lineSpacing;
            if (lineGap != o.lineGap) return lineGap < o.lineGap;
            if (bold != o.bold) return bold < o.bold;
            if (italic != o.italic) return italic < o.italic;
            return fontWeight < o.fontWeight;
        }
    };
    std::map<LayoutKey, TextLayoutResult> layouts_;
    FontManager fonts_;
};

// --- ResourceUploader: async uploads via the core ThreadPool ----------------
class ResourceUploader {
public:
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