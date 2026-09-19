#pragma once

// AssetLoader (docs/specs/13 §Asset Loader): responsible ONLY for turning
// metadata into a RuntimeAsset. Sync + async (via Core ThreadPool), with
// progress and cancellation. Text assets are decoded to a string form.

#include "modules/content/RuntimeAsset.hpp"
#include "core/common/Common.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace bps::content {

class IVfs;

struct LoadProgress {
    size_t bytesRead = 0;
    size_t totalBytes = 0;
    double fraction = 0.0;
};

using ProgressFn = std::function<void(const LoadProgress&)>;
using CancelToken = std::shared_ptr<std::atomic<bool>>;

inline CancelToken MakeCancelToken() { return std::make_shared<std::atomic<bool>>(false); }

class AssetLoader {
public:
    // `readBytes` resolves metadata → payload bytes (VFS lookup lives in the
    // ContentManager, which owns the mounts).
    using ReadFn = std::function<Result<std::vector<uint8_t>>(const AssetMetadata&)>;

    explicit AssetLoader(ReadFn readFn) : read_(std::move(readFn)) {}
    AssetLoader() = default;

    bool HasReadFn() const { return static_cast<bool>(read_); }
    void SetReadFn(ReadFn fn) { read_ = std::move(fn); }

    // Synchronous load (returns bytes only; callers build the RuntimeAsset).
    Result<std::vector<uint8_t>> Load(const AssetMetadata& meta, ProgressFn progress = {}) const;

    // Asynchronous load on the Core ThreadPool. The completion callback runs
    // on a pool worker. Cancellation via token checked between chunks.
    void LoadAsync(const AssetMetadata& meta, CancelToken cancel,
                   std::function<void(Result<std::vector<uint8_t>>, const LoadProgress&)> done,
                   ProgressFn progress = {}) const;

    // Best-effort content hash (FNV-1a 64, hex) used for duplicate detection.
    static std::string Hash(const uint8_t* data, size_t len);

private:
    ReadFn read_;
};

} // namespace bps::content
