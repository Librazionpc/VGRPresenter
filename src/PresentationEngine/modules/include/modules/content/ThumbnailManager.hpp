#pragma once

// ThumbnailManager (docs/specs/13 §Thumbnail Manager): generates thumbnails
// for assets. Ships with deterministic placeholder thumbnails (per-type SVG)
// and image dimension extraction from PNG/JPEG headers. Real raster decode is
// a documented future extension; the async pipeline (Core ThreadPool) is in
// place so decode-based generators can be added behind the same interface.

#include "modules/content/AssetMetadata.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bps::content {

struct ThumbnailSpec {
    int width = 96;
    int height = 96;
    std::string size = "small";   // "small" / "large"
};

using ThumbnailFn = std::function<void(const Uuid&, const std::string& /*size*/,
                                       const std::string& /*svg*/)>;

class ThumbnailManager {
public:
    ThumbnailManager() = default;

    // Synchronous placeholder generation (deterministic per type+uuid).
    std::string GeneratePlaceholder(const AssetMetadata& meta, const ThumbnailSpec& spec = {});

    // Extract image dimensions from PNG/JPEG header bytes; returns {w,h} or
    // {0,0} when the format isn't recognized. No full decode required.
    static std::pair<int, int> ImageDimensions(const uint8_t* data, size_t len);

    // Async generation on the Core ThreadPool; callback receives the SVG.
    void GenerateAsync(const AssetMetadata& meta, const ThumbnailSpec& spec,
                       ThumbnailFn done);

    // Cache of generated thumbnails (key: uuid + size).
    std::string Cached(const Uuid& uuid, std::string_view size) const;
    void ClearCache();

    size_t CacheCount() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> cache_;   // "uuid:size" -> svg
};

} // namespace bps::content
