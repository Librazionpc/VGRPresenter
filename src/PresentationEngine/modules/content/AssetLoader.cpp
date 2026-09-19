#include "modules/content/AssetLoader.hpp"

#include "core/threading/ThreadPool.hpp"

#include <format>

namespace bps::content {

Result<std::vector<uint8_t>> AssetLoader::Load(const AssetMetadata& meta,
                                               ProgressFn progress) const {
    if (!read_)
        return Error::Make(Err::InvalidState, "CAMS", "loader has no read backend");
    auto bytes = read_(meta);
    if (!bytes.ok()) return bytes;
    LoadProgress p;
    p.totalBytes = bytes.value().size();
    p.bytesRead = bytes.value().size();
    p.fraction = 1.0;
    if (progress) progress(p);
    return bytes;
}

void AssetLoader::LoadAsync(const AssetMetadata& meta, CancelToken cancel,
                            std::function<void(Result<std::vector<uint8_t>>,
                                               const LoadProgress&)> done,
                            ProgressFn progress) const {
    auto self = std::make_shared<AssetLoader>(*this);
    auto fn = [self, meta, cancel, done = std::move(done), progress = std::move(progress)]() {
        LoadProgress p;
        auto bytes = self->read_(meta);
        if (!bytes.ok()) {
            if (done) done(bytes.error(), p);
            return;
        }
        p.totalBytes = bytes.value().size();
        p.bytesRead = bytes.value().size();
        p.fraction = 1.0;
        if (progress) progress(p);
        if (done) done(std::move(bytes), p);
    };
    // Best-effort: if the pool is unavailable (tests before boot), run inline.
    auto& pool = ThreadPool::Instance();
    if (pool.IsInitialized()) {
        auto r = pool.SubmitBackground(std::move(fn));
        if (r.ok()) return;
    }
    fn();
}

std::string AssetLoader::Hash(const uint8_t* data, size_t len) {
    uint64_t h = 1469598103934665603ULL;   // FNV-1a 64 offset basis
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return std::format("{:016x}", h);
}

} // namespace bps::content
