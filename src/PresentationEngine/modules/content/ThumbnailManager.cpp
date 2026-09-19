#include "modules/content/ThumbnailManager.hpp"

#include "core/threading/ThreadPool.hpp"

#include <format>

namespace bps::content {

namespace {

// Deterministic per-type hue derived from the AssetType enum value.
const char* TypeColor(AssetType t) {
    switch (t) {
        case AssetType::Image:       return "#f97316";
        case AssetType::Video:       return "#ef4444";
        case AssetType::Audio:       return "#22c55e";
        case AssetType::Song:        return "#eab308";
        case AssetType::Bible:       return "#6366f1";
        case AssetType::Presentation:return "#0ea5e9";
        case AssetType::Font:        return "#14b8a6";
        case AssetType::Theme:       return "#a855f7";
        case AssetType::Text:
        case AssetType::Json:
        case AssetType::Data:        return "#64748b";
        default:                     return "#334155";
    }
}

std::string InitialFor(const std::string& name) {
    for (char c : name)
        if (c != ' ' && c != '.') return std::string(1, c);
    return "?";
}

} // namespace

std::string ThumbnailManager::GeneratePlaceholder(const AssetMetadata& meta,
                                                  const ThumbnailSpec& spec) {
    std::string svg = std::format(
        "<svg xmlns='http://www.w3.org/2000/svg' width='{}' height='{}'>"
        "<rect width='100%' height='100%' fill='{}'/>"
        "<text x='50%' y='55%' fill='white' font-family='sans-serif' "
        "font-size='{}' text-anchor='middle'>{}</text>"
        "<text x='50%' y='90%' fill='rgba(255,255,255,0.7)' "
        "font-family='monospace' font-size='8' text-anchor='middle'>{}</text>"
        "</svg>",
        spec.width, spec.height, TypeColor(meta.type),
        spec.width / 2, InitialFor(meta.name), ToString(meta.type));
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[meta.uuid.ToString() + ":" + spec.size] = svg;
    return svg;
}

std::pair<int, int> ThumbnailManager::ImageDimensions(const uint8_t* data, size_t len) {
    // PNG: signature + IHDR at offset 16 (width BE32, height BE32).
    if (len >= 24 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
        data[12] == 'I' && data[13] == 'H' && data[14] == 'D' && data[15] == 'R') {
        int w = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];
        int h = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];
        return {w, h};
    }
    // JPEG: scan for SOF0/SOF2 markers (0xFFC0 / 0xFFC2).
    if (len >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < len) {
            if (data[i] != 0xFF) { ++i; continue; }
            uint8_t marker = data[i + 1];
            if (marker == 0xC0 || marker == 0xC2 || marker == 0xC1 ||
                marker == 0xC3 || marker == 0xC5 || marker == 0xC6 ||
                marker == 0xC7 || marker == 0xC9 || marker == 0xCA ||
                marker == 0xCB || marker == 0xCD || marker == 0xCE ||
                marker == 0xCF) {
                int h = (data[i + 5] << 8) | data[i + 6];
                int w = (data[i + 7] << 8) | data[i + 8];
                return {w, h};
            }
            if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) {
                i += 2;
                continue;
            }
            if (i + 3 < len) {
                uint16_t seg = (data[i + 2] << 8) | data[i + 3];
                if (seg < 2) break;
                i += 2 + seg;
            } else {
                break;
            }
        }
    }
    return {0, 0};
}

void ThumbnailManager::GenerateAsync(const AssetMetadata& meta, const ThumbnailSpec& spec,
                                     ThumbnailFn done) {
    std::string key = meta.uuid.ToString() + ":" + spec.size;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.count(key)) {
            if (done) done(meta.uuid, spec.size, cache_[key]);
            return;
        }
    }
    auto self = this;
    auto fn = [self, meta, spec, done = std::move(done)]() {
        std::string svg = self->GeneratePlaceholder(meta, spec);
        if (done) done(meta.uuid, spec.size, svg);
    };
    auto& pool = ThreadPool::Instance();
    if (pool.IsInitialized()) {
        (void)pool.SubmitBackground(std::move(fn));
        return;
    }
    fn();
}

std::string ThumbnailManager::Cached(const Uuid& uuid, std::string_view size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(uuid.ToString() + ":" + std::string(size));
    return it == cache_.end() ? std::string{} : it->second;
}

void ThumbnailManager::ClearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

size_t ThumbnailManager::CacheCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

} // namespace bps::content
