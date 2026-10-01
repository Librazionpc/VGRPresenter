#include "modules/rendering/GpuResources.hpp"

#include "core/threading/ThreadPool.hpp"

#include <algorithm>

namespace bps::rendering {

// ===========================================================================
// GPUResourceManager
// ===========================================================================

Result<TextureId> GPUResourceManager::AcquireTexture(std::string_view name,
                                                     const RgbaImage& image) {
    if (!backend_) return Error::Make(Err::Render_BackendUnavailable, "GPU", "no backend");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(name);
    if (it != textures_.end()) {
        ++it->second.refs;
        return it->second.id;
    }
    if (textureBudget_ > 0 && image.ByteSize() > textureBudget_)
        return Error::Make(Err::Render_TextureAllocFailed, "GPU", "over texture budget");
    auto id = backend_->CreateTexture(image);
    if (!id.ok())
        return Error::Make(Err::Render_TextureAllocFailed, "GPU", id.error().message);
    Entry e;
    e.id = id.value();
    e.name = std::string(name);
    e.bytes = image.ByteSize();
    textures_[std::string(name)] = std::move(e);
    return id.value();
}

Result<void> GPUResourceManager::ReleaseTexture(std::string_view name) {
    if (!backend_) return Error::Make(Err::Render_BackendUnavailable, "GPU", "no backend");
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(name);
    if (it == textures_.end())
        return Error::Make(Err::Render_TextureNotFound, "GPU", "no texture '" + std::string(name) + "'");
    if (--it->second.refs <= 0) {
        (void)backend_->DestroyTexture(it->second.id);
        textures_.erase(it);
    }
    return Ok();
}

Result<TextureId> GPUResourceManager::GetTexture(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = textures_.find(name);
    if (it == textures_.end())
        return Error::Make(Err::Render_TextureNotFound, "GPU", "no texture '" + std::string(name) + "'");
    return it->second.id;
}

size_t GPUResourceManager::TextureCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return textures_.size();
}

Result<uint32_t> GPUResourceManager::AcquireShader(std::string_view name,
                                                   std::string_view source) {
    if (!backend_) return Error::Make(Err::Render_BackendUnavailable, "GPU", "no backend");
    return backend_->CreateShader(name, source);
}

size_t GPUResourceManager::EvictIdle(uint64_t keepBytes) {
    if (!backend_) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> drop;
    uint64_t current = backend_->TextureMemoryBytes();
    for (const auto& [name, e] : textures_) {
        if (e.refs > 0) continue;   // referenced resources stay
        if (current <= keepBytes) break;
        drop.push_back(name);
        current -= e.bytes;
    }
    for (const auto& name : drop) {
        auto it = textures_.find(name);
        (void)backend_->DestroyTexture(it->second.id);
        textures_.erase(it);
    }
    return drop.size();
}

void GPUResourceManager::Clear() {
    if (!backend_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, e] : textures_) {
        (void)name;
        (void)backend_->DestroyTexture(e.id);
    }
    textures_.clear();
}

// ===========================================================================
// RenderCache
// ===========================================================================

Result<const RenderCache::AtlasEntry*> RenderCache::GlyphAtlas(float sizePx) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int key = static_cast<int>(sizePx + 0.5f);
    auto it = atlases_.find(key);
    if (it != atlases_.end()) return it->second.get();

    auto entry = std::make_unique<AtlasEntry>();
    entry->atlas = fonts_.BuildAtlas(sizePx, entry->glyphs);
    auto* raw = entry.get();
    atlases_[key] = std::move(entry);
    return raw;
}

Result<const RenderCache::AtlasEntry*> RenderCache::GlyphAtlas(const TextStyle& style,
                                                                const FontManager& fonts) {
    std::lock_guard<std::mutex> lock(mutex_);
    const FontAtlasKey key{style.fontId, static_cast<int>(style.size + 0.5f),
                           style.bold, style.italic};
    auto it = fontAtlases_.find(key);
    if (it != fontAtlases_.end()) return it->second.get();

    auto entry = std::make_unique<AtlasEntry>();
    // Real font first; an empty result (family unresolved, or "builtin"/""
    // fontId — the common case for every style that never named a real
    // font at all) falls back to the SAME builtin atlas every other caller
    // already gets, so a style with no real font behaves exactly as before
    // this cache existed.
    if (!style.fontId.empty() && style.fontId != "builtin" && fonts.HasSystemFont(style.fontId))
        entry->atlas = fonts.BuildSystemAtlas(style.fontId, style.size, style.bold,
                                              style.italic, entry->glyphs);
    if (entry->atlas.empty())
        entry->atlas = fonts.BuildAtlas(style.size, entry->glyphs);
    auto* raw = entry.get();
    fontAtlases_[key] = std::move(entry);
    return raw;
}

void RenderCache::InvalidateGlyphAtlas() {
    std::lock_guard<std::mutex> lock(mutex_);
    atlases_.clear();
    fontAtlases_.clear();
}

Result<TextLayoutResult> RenderCache::Layout(const std::string& text,
                                             const TextStyle& style,
                                             const FontManager& fonts) {
    std::lock_guard<std::mutex> lock(mutex_);
    LayoutKey key;
    key.text = text;
    key.font = style.fontId;
    key.size = style.size;
    key.wrap = style.wrapWidth;
    key.wrapOn = style.wrap;
    key.letterSpacing = style.letterSpacing;
    key.lineSpacing = style.lineSpacing;
    key.bold = style.bold;
    key.italic = style.italic;
    auto it = layouts_.find(key);
    if (it != layouts_.end()) return it->second;
    auto result = TextLayout::Measure(text, style, fonts);
    layouts_[key] = result;
    return result;
}

void RenderCache::InvalidateLayouts() {
    std::lock_guard<std::mutex> lock(mutex_);
    layouts_.clear();
}

size_t RenderCache::LayoutCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return layouts_.size();
}

size_t RenderCache::AtlasCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return atlases_.size() + fontAtlases_.size();
}

void RenderCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    atlases_.clear();
    fontAtlases_.clear();
    layouts_.clear();
}

// ===========================================================================
// ResourceUploader
// ===========================================================================

void ResourceUploader::Submit(std::string name, std::vector<uint8_t> bytes,
                              Decoder decoder, DoneFn done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job job;
        job.name = std::move(name);
        job.bytes = std::move(bytes);
        job.decoder = std::move(decoder);
        job.done = std::move(done);
        queued_.push_back(std::move(job));
    }
    // Decode on the core thread pool; the render thread only sees the result
    // via Pump() — it never blocks on decoding. If the pool is unavailable
    // (shutdown/not yet booted), decode inline as a graceful fallback so the
    // job always completes and `done` always fires.
    auto decodeJob = [this](Job job) {
        RgbaImage img = job.decoder ? job.decoder(job.bytes) : RgbaImage{};
        Decoded d;
        d.name = std::move(job.name);
        d.image = std::move(img);
        d.done = std::move(job.done);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            decoded_.push_back(std::move(d));
        }
    };
    auto job = [this, decodeJob]() {
        Job j;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queued_.empty()) return;
            j = std::move(queued_.front());
            queued_.erase(queued_.begin());
        }
        decodeJob(std::move(j));
    };
    auto r = ThreadPool::Instance().Submit(job);
    if (!r.ok()) {
        // Pool unavailable: pop and decode inline now.
        Job j;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queued_.empty()) {
                j = std::move(queued_.front());
                queued_.erase(queued_.begin());
            }
        }
        if (j.decoder) decodeJob(std::move(j));
    }
}

void ResourceUploader::Pump() {
    std::vector<Decoded> take;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        take.swap(decoded_);
    }
    for (auto& d : take) {
        Result<TextureId> id = Error::Make(Err::Render_TextureAllocFailed, "Upload",
                                           "decoder produced no image for '" + d.name + "'");
        if (gpu_ && !d.image.empty()) {
            auto r = gpu_->AcquireTexture(d.name, d.image);
            if (r.ok()) id = r.value();
        }
        if (d.done) d.done(id);
    }
}

size_t ResourceUploader::Pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_.size() + decoded_.size();
}

void ResourceUploader::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    queued_.clear();
    decoded_.clear();
}

// ===========================================================================
// FrameGraph
// ===========================================================================

void FrameGraph::BeginFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frame_;
}

void FrameGraph::Use(std::string_view name, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& e = resources_[std::string(name)];
    e.bytes = bytes;
    e.unusedFrames = 0;
}

void FrameGraph::EndFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, e] : resources_) {
        (void)name;
        ++e.unusedFrames;
    }
}

std::vector<std::string> FrameGraph::IdleResources(int idleFrames) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, e] : resources_)
        if (e.unusedFrames >= idleFrames) out.push_back(name);
    return out;
}

size_t FrameGraph::LiveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resources_.size();
}

} // namespace bps::rendering
