#include "modules/content/ContentManager.hpp"

#include "modules/content/AssetSerializer.hpp"
#include "core/config/ConfigurationManager.hpp"
#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/resources/ResourceManager.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "core/threading/ThreadPool.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace bps::content {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

ContentManager& ContentManager::Instance() {
    static ContentManager instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<void> ContentManager::Initialize() {
    if (initialized_.load()) return Ok();
    auto& logger = Logger::Instance();

    auto& config = ConfigurationManager::Instance();
    size_t cacheBytes = static_cast<size_t>(
        config.GetInt("cams.cache.bytes", 64LL * 1024LL * 1024LL));
    assets_.SetCapacity(cacheBytes);
    assets_.SetLoader(AssetLoader([this](const AssetMetadata& m) { return ReadViaVfs(m); }));

    // Register the built-in importers/exporters.
    if (auto r = importers_.Register(std::make_shared<TextImporter>()); !r.ok()) return r;
    if (auto r = importers_.Register(std::make_shared<JsonImporter>()); !r.ok()) return r;
    if (auto r = importers_.Register(std::make_shared<ImageImporter>()); !r.ok()) return r;
    if (auto r = exporters_.Register(std::make_shared<TextExporter>()); !r.ok()) return r;
    if (auto r = exporters_.Register(std::make_shared<JsonExporter>()); !r.ok()) return r;
    if (auto r = exporters_.Register(std::make_shared<PackageExporter>()); !r.ok()) return r;

    // Restore persisted metadata (content collection in the DatabaseManager).
    if (auto r = LoadLibrary(); !r.ok())
        logger.Warning("CAMS: metadata restore incomplete: " + r.error().message, "CAMS");

    // EventBus: consume shutdown + hot reload + memory pressure. Tokens are
    // retained so Shutdown() can unsubscribe (no handler pile-up across re-inits).
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ShutdownStarted>(
        [this](const events::ShutdownStarted&) { (void)Stop(); }));
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload&) { (void)Reload(); }));
    subscriptions_.push_back(bus.Subscribe<events::ResourcePressureHigh>(
        [this](const events::ResourcePressureHigh&) { OnPressureHigh(); }));

    initialized_.store(true);
    logger.Info(std::format("ContentManager ready: {} importers, {} exporters, {} assets",
                             importers_.ImporterCount(), exporters_.ExporterCount(),
                             db_.Count()),
                "CAMS");
    return Ok();
}

Result<void> ContentManager::Start() {
    if (!initialized_.load()) return Error::Make(Err::InvalidState, "CAMS", "not initialized");
    if (started_.load()) return Ok();   // idempotent: one heartbeat only
    started_.store(true);
    // Schedule the watcher heartbeat (no module owns a thread).
    auto& sched = TaskScheduler::Instance();
    auto r = sched.ScheduleEvery([this]() { PollWatch(); }, std::chrono::seconds(1));
    if (r.ok()) heartbeatTask_ = r.value();
    return Ok();
}

Result<void> ContentManager::Stop() {
    started_.store(false);
    if (heartbeatTask_ != 0) {
        (void)TaskScheduler::Instance().Cancel(heartbeatTask_);
        heartbeatTask_ = 0;
    }
    StopWatch();
    return Ok();
}Result<void> ContentManager::Shutdown() {
    if (!initialized_.load()) return Ok();
    (void)Stop();
    // Release EventBus subscriptions so a later Initialize() doesn't double up.
    auto& bus = EventBus::Instance();
    for (auto& sub : subscriptions_) {
        if (sub.Valid()) (void)bus.Unsubscribe(sub);
    }
    subscriptions_.clear();
    if (auto r = PersistLibrary(); !r.ok())
        Logger::Instance().Warning("CAMS: metadata save on shutdown failed: " +
                                       r.error().message, "CAMS");
    (void)assets_.UnloadAll();
    db_.Clear();
    indexer_.Clear();
    thumbnails_.ClearCache();
    importers_.Clear();   // re-registered by the next Initialize()
    exporters_.Clear();
    {
        std::lock_guard<std::mutex> lock(mountMutex_);
        mounts_.clear();
    }
    initialized_.store(false);
    return Ok();
}

Result<void> ContentManager::Reload() {
    if (!initialized_.load()) return Ok();
    auto& config = ConfigurationManager::Instance();
    size_t cacheBytes = static_cast<size_t>(
        config.GetInt("cams.cache.bytes", 64LL * 1024LL * 1024LL));
    assets_.SetCapacity(cacheBytes);
    // Rebuild the search index from the database on config reload.
    ReindexAll();
    Logger::Instance().Info(std::format("ContentManager reloaded (cache={} bytes)", cacheBytes),
                            "CAMS");
    return Ok();
}

Result<void> ContentManager::Reset() {
    (void)Stop();
    (void)assets_.UnloadAll();
    db_.Clear();
    indexer_.Clear();
    thumbnails_.ClearCache();
    importers_.Clear();
    exporters_.Clear();
    {
        std::lock_guard<std::mutex> lock(mountMutex_);
        mounts_.clear();
    }
    auto& db = DatabaseManager::Instance();
    for (const auto& key : db.Keys("content")) (void)db.Remove("content", key);
    initialized_.store(false);
    return Ok();
}

HealthReport ContentManager::GetHealth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    HealthReport h;
    h.state = errorCount_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    h.detail = std::format("{} assets, {} loaded, {} importers", db_.Count(),
                           assets_.LoadedCount(), importers_.ImporterCount());
    h.lastError = lastError_;
    h.errorCount = errorCount_.load();
    return h;
}

Metrics ContentManager::MetricsSnapshot() const {
    Metrics m;
    m.ramBytes = 0;
    CacheStats cs = assets_.GetCacheStats();
    m.queueLength = cs.entries;
    m.errorCount = errorCount_.load();
    m.health = errorCount_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    std::lock_guard<std::mutex> lock(mutex_);
    if (operations_ > 0)
        m.latencyP95 = std::chrono::microseconds{opLatencySum_.count() / static_cast<long long>(operations_)};
    return m;
}

// ---------------------------------------------------------------------------
// Mounts
// ---------------------------------------------------------------------------

std::shared_ptr<IVfs> ContentManager::FindMount(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mountMutex_);
    auto it = mounts_.find(std::string(name));
    return it == mounts_.end() ? nullptr : it->second;
}

std::string ContentManager::MountPrefix(std::string_view name) const {
    return std::string(name) + ":/";
}

Result<void> ContentManager::MountDisk(std::string name, std::string hostDir) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (!fs.Exists(hostDir)) {
        if (auto r = fs.CreateDirectories(hostDir); !r.ok()) return r;
    }
    auto vfs = std::make_shared<DiskVfs>(name, std::move(hostDir));
    std::lock_guard<std::mutex> lock(mountMutex_);
    mounts_[std::move(name)] = std::move(vfs);
    return Ok();
}

Result<std::shared_ptr<IVfs>> ContentManager::MountMemory(std::string name) {
    auto vfs = std::make_shared<MemoryVfs>(name);
    std::lock_guard<std::mutex> lock(mountMutex_);
    mounts_[std::move(name)] = vfs;
    return Result<std::shared_ptr<IVfs>>{vfs};
}

Result<std::shared_ptr<IVfs>> ContentManager::MountZip(std::string name,
                                                       std::string archivePath) {
    auto vfs = std::make_shared<ZipVfs>(name, std::move(archivePath));
    if (auto r = vfs->Open(); !r.ok()) return r.error();
    std::lock_guard<std::mutex> lock(mountMutex_);
    mounts_[std::move(name)] = vfs;
    return Result<std::shared_ptr<IVfs>>{vfs};
}

Result<void> ContentManager::Unmount(std::string_view name) {
    std::lock_guard<std::mutex> lock(mountMutex_);
    auto it = mounts_.find(std::string(name));
    if (it == mounts_.end())
        return Error::Make(Err::NotFound, "CAMS", "no such mount: " + std::string(name));
    mounts_.erase(it);
    return Ok();
}

std::vector<std::string> ContentManager::MountNames() const {
    std::lock_guard<std::mutex> lock(mountMutex_);
    std::vector<std::string> out;
    for (const auto& [n, v] : mounts_) {
        (void)v;
        out.push_back(n);
    }
    return out;
}

Result<std::vector<uint8_t>> ContentManager::ReadViaVfs(const AssetMetadata& meta) const {
    auto vfs = FindMount(meta.vfs);
    if (!vfs)
        return Error::Make(Err::Content_VfsPathNotMounted, "CAMS",
                           "asset references unmounted vfs '" + meta.vfs + "'");
    return vfs->Read(meta.path);
}

// ---------------------------------------------------------------------------
// Library operations
// ---------------------------------------------------------------------------

Result<Uuid> ContentManager::ImportFile(std::string hostPath, const ImportOptions& opts) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto bytes = fs.ReadBinary(hostPath);
    if (!bytes.ok()) return bytes.error();
    auto dot = hostPath.find_last_of('/');
    std::string fileName = dot == std::string::npos ? hostPath : hostPath.substr(dot + 1);
    ImportContext ctx;
    ctx.source = hostPath;
    ctx.fileName = fileName;
    ctx.destinationDir = opts.destinationDir;
    ctx.suggestedName = opts.suggestedName;
    ctx.tags = opts.tags;
    ctx.category = opts.category;
    ctx.author = opts.author;
    ctx.generateThumbnail = opts.generateThumbnail;
    ctx.cancelled = []() { return false; };

    auto result = importers_.Import(fileName, bytes.value(), ctx);
    if (!result.ok()) return result.error();
    if (result.value().assets.empty())
        return Error::Make(Err::Content_ImportFailed, "CAMS", "importer produced no assets");
    if (!result.value().warnings.empty())
        Logger::Instance().Info(
            std::format("CAMS: import warnings: {}", result.value().warnings.size()), "CAMS");

    // Write into the library (default mount "lib") and register metadata.
    auto lib = FindMount("lib");
    if (!lib)
        return Error::Make(Err::Content_VfsPathNotMounted, "CAMS", "no 'lib' mount mounted");
    Uuid firstUuid{};
    for (auto& a : result.value().assets) {
        a.meta.vfs = "lib";
        a.meta.sizeBytes = a.bytes.size();
        a.meta.hash = AssetLoader::Hash(a.bytes.data(), a.bytes.size());
        a.meta.checksum = std::format("{:08x}", Crc32(a.bytes.data(), a.bytes.size()));
        int64_t now = NowMs();
        a.meta.createdMs = now;
        a.meta.modifiedMs = now;
        a.meta.state = AssetState::Imported;
        if (auto r = lib->Write(a.meta.path, a.bytes); !r.ok()) return r.error();
        if (auto r = db_.Upsert(a.meta); !r.ok()) return r.error();
        indexer_.Index(a.meta);
        if (opts.generateThumbnail) thumbnails_.GeneratePlaceholder(a.meta);
        if (firstUuid == Uuid{}) firstUuid = a.meta.uuid;
        (void)EventBus::Instance().Publish(events::ContentAssetAdded{
            a.meta.uuid.ToString(), a.meta.name});
        Logger::Instance().Info(std::format("CAMS: imported {} ({}, {} bytes)", a.meta.name,
                                            ToString(a.meta.type), a.bytes.size()),
                                "CAMS");
    }
    (void)EventBus::Instance().Publish(events::ContentAssetImported{
        firstUuid.ToString(), result.value().assets.front().meta.name,
        result.value().importerName, result.value().assets.size()});
    return Result<Uuid>{firstUuid};
}

Result<Uuid> ContentManager::CreateText(std::string name, AssetType type, std::string text,
                                        const ImportOptions& opts) {
    if (type != AssetType::Text && type != AssetType::Json && type != AssetType::Data)
        return Error::Make(Err::InvalidArgument, "CAMS", "CreateText requires a text-like type");
    auto lib = FindMount("lib");
    if (!lib)
        return Error::Make(Err::Content_VfsPathNotMounted, "CAMS", "no 'lib' mount mounted");
    AssetMetadata meta;
    meta.uuid = Uuid::Generate();
    meta.name = name;
    meta.type = type;
    meta.vfs = "lib";
    meta.path = opts.destinationDir.empty() ? name : opts.destinationDir + "/" + name;
    std::vector<uint8_t> bytes(text.begin(), text.end());
    meta.sizeBytes = bytes.size();
    meta.hash = AssetLoader::Hash(bytes.data(), bytes.size());
    meta.checksum = std::format("{:08x}", Crc32(bytes.data(), bytes.size()));
    int64_t now = NowMs();
    meta.createdMs = now;
    meta.modifiedMs = now;
    meta.tags = opts.tags;
    meta.category = opts.category;
    meta.author = opts.author;
    meta.state = AssetState::Imported;

    if (auto r = lib->Write(meta.path, bytes); !r.ok()) return r.error();
    if (auto r = db_.Upsert(meta); !r.ok()) return r.error();
    indexer_.Index(meta);
    thumbnails_.GeneratePlaceholder(meta);
    (void)EventBus::Instance().Publish(events::ContentAssetAdded{meta.uuid.ToString(), meta.name});
    return Result<Uuid>{meta.uuid};
}

Result<AssetMetadata> ContentManager::GetMetadata(const Uuid& uuid) const {
    return db_.Get(uuid);
}

std::vector<AssetMetadata> ContentManager::List() const { return db_.All(); }

size_t ContentManager::AssetCount() const { return db_.Count(); }

std::optional<AssetMetadata> ContentManager::FindByPath(std::string_view path) const {
    return db_.FindByPath(path);
}

// ---------------------------------------------------------------------------
// Runtime assets
// ---------------------------------------------------------------------------

Result<std::shared_ptr<RuntimeAsset>> ContentManager::Load(const Uuid& uuid) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    return Load(meta.value());
}

Result<std::shared_ptr<RuntimeAsset>> ContentManager::Load(const AssetMetadata& meta) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = assets_.Load(meta);
    auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0);
    std::lock_guard<std::mutex> lock(mutex_);
    ++operations_;
    opLatencySum_ += dt;
    if (!r.ok()) {
        ++errorCount_;
        lastError_ = r.error().message;
        return r;
    }
    (void)EventBus::Instance().Publish(
        events::ContentAssetLoaded{meta.uuid.ToString(), meta.name, r.value()->bytes.size()});
    return r;
}

void ContentManager::LoadAsync(
    const Uuid& uuid, CancelToken cancel,
    std::function<void(Result<std::shared_ptr<RuntimeAsset>>)> done) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) {
        if (done) done(meta.error());
        return;
    }
    // Fast path: already cached.
    if (auto hit = assets_.Peek(uuid); hit) {
        if (done) done(Result<std::shared_ptr<RuntimeAsset>>{hit});
        return;
    }
    auto self = this;
    auto metaCopy = meta.value();
    auto load = [self, metaCopy, cancel, done = std::move(done)]() mutable {
        if (cancel && cancel->load()) {
            if (done) done(Error::Make(Err::Content_Cancelled, "CAMS", "load cancelled"));
            return;
        }
        auto r = self->Load(metaCopy);
        if (done) done(r);
    };
    auto& pool = ThreadPool::Instance();
    if (pool.IsInitialized()) {
        auto hr = pool.SubmitBackground(std::move(load));
        if (hr.ok()) return;
    }
    load();
}

Result<void> ContentManager::Unload(const Uuid& uuid) {
    if (auto r = assets_.Unload(uuid); !r.ok()) return r;
    (void)EventBus::Instance().Publish(events::ContentAssetUnloaded{
        uuid.ToString(), "unloaded"});
    return Ok();
}

Result<void> ContentManager::Pin(const Uuid& uuid, bool pinned) {
    return assets_.Pin(uuid, pinned);
}

Result<void> ContentManager::Prefetch(const Uuid& uuid) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    assets_.Prefetch(meta.value());
    return Ok();
}

std::shared_ptr<RuntimeAsset> ContentManager::Peek(const Uuid& uuid) const {
    return assets_.Peek(uuid);
}

size_t ContentManager::LoadedCount() const { return assets_.LoadedCount(); }

// ---------------------------------------------------------------------------
// Content operations
// ---------------------------------------------------------------------------

Result<std::string> ContentManager::ReadText(const Uuid& uuid) {
    auto a = Load(uuid);
    if (!a.ok()) return a.error();
    return Result<std::string>{a.value()->text};
}

Result<std::vector<uint8_t>> ContentManager::ReadBytes(const Uuid& uuid) {
    auto a = Load(uuid);
    if (!a.ok()) return a.error();
    return Result<std::vector<uint8_t>>{a.value()->bytes};
}

Result<void> ContentManager::Save(const Uuid& uuid, std::vector<uint8_t> bytes) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    auto vfs = FindMount(meta.value().vfs);
    if (!vfs) return Error::Make(Err::Content_VfsPathNotMounted, "CAMS",
                                 "asset references unmounted vfs '" + meta.value().vfs + "'");
    if (auto r = vfs->Write(meta.value().path, bytes); !r.ok()) return r;
    auto updated = meta.value();
    updated.sizeBytes = bytes.size();
    updated.hash = AssetLoader::Hash(bytes.data(), bytes.size());
    updated.checksum = std::format("{:08x}", Crc32(bytes.data(), bytes.size()));
    updated.modifiedMs = NowMs();
    updated.state = AssetState::Saved;
    if (auto r = db_.Upsert(updated); !r.ok()) return r.error();
    indexer_.Index(updated);
    assets_.Refresh(uuid, bytes, updated);
    (void)EventBus::Instance().Publish(events::ContentAssetChanged{
        uuid.ToString(), updated.name});
    return Ok();
}

Result<void> ContentManager::SaveText(const Uuid& uuid, std::string text) {
    return Save(uuid, std::vector<uint8_t>(text.begin(), text.end()));
}

Result<void> ContentManager::Delete(const Uuid& uuid) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    auto vfs = FindMount(meta.value().vfs);
    if (vfs) (void)vfs->Remove(meta.value().path);
    if (auto r = db_.Remove(uuid); !r.ok()) return r;
    indexer_.Remove(uuid);
    (void)assets_.Unload(uuid);
    (void)EventBus::Instance().Publish(events::ContentAssetDeleted{uuid.ToString()});
    (void)EventBus::Instance().Publish(events::ContentAssetRemoved{uuid.ToString()});
    return Ok();
}

Result<void> ContentManager::Move(const Uuid& uuid, std::string_view newPath) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    auto vfs = FindMount(meta.value().vfs);
    if (!vfs) return Error::Make(Err::Content_VfsPathNotMounted, "CAMS",
                                 "asset references unmounted vfs '" + meta.value().vfs + "'");
    if (auto r = vfs->Move(meta.value().path, newPath); !r.ok()) return r.error();
    auto updated = meta.value();
    updated.path = std::string(newPath);
    updated.modifiedMs = NowMs();
    if (auto r = db_.Upsert(updated); !r.ok()) return r.error();
    indexer_.Index(updated);
    (void)EventBus::Instance().Publish(events::ContentAssetChanged{
        uuid.ToString(), updated.name});
    return Ok();
}

Result<void> ContentManager::Rename(const Uuid& uuid, std::string_view newName) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    auto vfs = FindMount(meta.value().vfs);
    if (!vfs) return Error::Make(Err::Content_VfsPathNotMounted, "CAMS",
                                 "asset references unmounted vfs '" + meta.value().vfs + "'");
    // Keep name + path in sync: rename is a Move within the same directory.
    auto oldPath = meta.value().path;
    auto slash = oldPath.find_last_of('/');
    std::string dir = slash == std::string::npos ? std::string{} : oldPath.substr(0, slash + 1);
    std::string newPath = dir + std::string(newName);
    // If the caller omitted the extension, preserve the original one.
    auto lastDot = newPath.find_last_of('.');
    if (lastDot == std::string::npos || lastDot < newPath.find_last_of('/')) {
        auto oldDot = oldPath.find_last_of('.');
        if (oldDot != std::string::npos && oldDot > slash)
            newPath += oldPath.substr(oldDot);
    }
    if (newPath != oldPath) {
        if (auto r = vfs->Move(oldPath, newPath); !r.ok()) return r.error();
    }
    auto updated = meta.value();
    updated.path = newPath;
    updated.name = std::string(newName);
    updated.modifiedMs = NowMs();
    if (auto r = db_.Upsert(updated); !r.ok()) return r.error();
    indexer_.Index(updated);
    (void)EventBus::Instance().Publish(events::ContentAssetChanged{
        uuid.ToString(), updated.name});
    return Ok();
}

Result<Uuid> ContentManager::Duplicate(const Uuid& uuid) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    auto vfs = FindMount(meta.value().vfs);
    if (!vfs) return Error::Make(Err::Content_VfsPathNotMounted, "CAMS",
                                 "asset references unmounted vfs '" + meta.value().vfs + "'");
    auto bytes = vfs->Read(meta.value().path);
    if (!bytes.ok()) return bytes.error();

    AssetMetadata copy = meta.value();
    copy.uuid = Uuid::Generate();
    auto dot = copy.path.find_last_of('.');
    std::string stem = dot == std::string::npos ? copy.path : copy.path.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : copy.path.substr(dot);
    copy.path = stem + " copy" + ext;
    copy.name += " copy";
    copy.createdMs = NowMs();
    copy.modifiedMs = NowMs();
    copy.state = AssetState::Imported;
    if (auto r = vfs->Write(copy.path, bytes.value()); !r.ok()) return r.error();
    if (auto r = db_.Upsert(copy); !r.ok()) return r.error();
    indexer_.Index(copy);
    thumbnails_.GeneratePlaceholder(copy);
    (void)EventBus::Instance().Publish(events::ContentAssetAdded{copy.uuid.ToString(), copy.name});
    return Result<Uuid>{copy.uuid};
}

Result<void> ContentManager::Export(const Uuid& uuid, std::string_view format,
                                    std::string hostDestPath) {
    auto meta = db_.Get(uuid);
    if (!meta.ok()) return meta.error();
    auto bytes = ReadViaVfs(meta.value());
    if (!bytes.ok()) return bytes.error();
    ExportContext ctx;
    ctx.meta = meta.value();
    ctx.bytes = bytes.value();
    ctx.destinationPath = std::move(hostDestPath);
    ctx.progress = [](double) {};
    ctx.cancelled = []() { return false; };
    auto r = exporters_.Export(format, ctx);
    if (r.ok())
        (void)EventBus::Instance().Publish(events::ContentAssetExported{
            uuid.ToString(), std::string(format), ctx.destinationPath});
    else {
        ++errorCount_;
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = r.error().message;
    }
    return r;
}

Result<void> ContentManager::ExportPackage(const std::vector<Uuid>& uuids,
                                           std::string hostDestPath) {
    ZipWriter writer;
    for (const auto& u : uuids) {
        auto meta = db_.Get(u);
        if (!meta.ok()) return meta.error();
        auto bytes = ReadViaVfs(meta.value());
        if (!bytes.ok()) return bytes.error();
        if (auto r = writer.AddEntry(meta.value().path, bytes.value()); !r.ok()) return r;
    }
    auto r = writer.WriteTo(hostDestPath);
    if (r.ok())
        (void)EventBus::Instance().Publish(events::ContentAssetExported{
            "package", "zip", hostDestPath});
    return r;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

std::vector<SearchResult> ContentManager::Search(const SearchQuery& q) const {
    return indexer_.Search(q);
}

void ContentManager::ReindexAll() {
    indexer_.Clear();
    for (const auto& m : db_.All()) indexer_.Index(m);
}

// ---------------------------------------------------------------------------
// Extension points
// ---------------------------------------------------------------------------

Result<void> ContentManager::RegisterImporter(std::shared_ptr<IImporter> importer) {
    return importers_.Register(std::move(importer));
}

Result<void> ContentManager::RegisterExporter(std::shared_ptr<IExporter> exporter) {
    return exporters_.Register(std::move(exporter));
}

std::vector<std::string> ContentManager::SupportedImportExtensions() const {
    return importers_.SupportedExtensions();
}

std::vector<std::string> ContentManager::SupportedExportFormats() const {
    return exporters_.SupportedFormats();
}

// ---------------------------------------------------------------------------
// Cache control
// ---------------------------------------------------------------------------

CacheStats ContentManager::GetCacheStats() const { return assets_.GetCacheStats(); }

size_t ContentManager::Evict() { return assets_.EvictAllUnpinned(); }

size_t ContentManager::ShrinkCache(double fraction) { return assets_.ShrinkCache(fraction); }

void ContentManager::SetCacheCapacity(size_t bytes) { assets_.SetCapacity(bytes); }

// ---------------------------------------------------------------------------
// Watcher
// ---------------------------------------------------------------------------

Result<void> ContentManager::WatchRoot(std::string_view mountName, std::string_view dir) {
    auto vfs = FindMount(mountName);
    if (!vfs) return Error::Make(Err::Content_VfsPathNotMounted, "CAMS",
                                 "no such mount: " + std::string(mountName));
    std::string prefix = std::string(mountName) + ":/";
    std::string mountStr(mountName);
    watcher_.SetCallback([this, prefix, vfs, mountStr](const WatchChange& c) {
        auto registerFromVfs = [this, vfs, &prefix, &mountStr](const std::string& rel) {
            // Read bytes through the VFS (never the host FS) and register a
            // metadata record for a file that appeared in a watched root.
            auto bytes = vfs->Read(rel);
            if (!bytes.ok()) return;
            auto existing = db_.FindByPath(prefix + rel);
            if (existing) return;
            auto dot = rel.find_last_of('.');
            std::string ext = dot == std::string::npos ? "" : rel.substr(dot + 1);
            AssetMetadata meta;
            meta.uuid = Uuid::Generate();
            meta.name = rel.substr(rel.find_last_of('/') + 1);
            meta.type = TypeForExtension(ext);
            meta.vfs = mountStr;
            meta.path = rel;
            meta.sizeBytes = bytes.value().size();
            meta.hash = AssetLoader::Hash(bytes.value().data(), bytes.value().size());
            meta.checksum = std::format("{:08x}",
                                        Crc32(bytes.value().data(), bytes.value().size()));
            int64_t now = NowMs();
            meta.createdMs = now;
            meta.modifiedMs = now;
            meta.state = AssetState::Discovered;
            if (auto r = db_.Upsert(meta); r.ok()) {
                indexer_.Index(meta);
                (void)EventBus::Instance().Publish(events::ContentAssetAdded{
                    meta.uuid.ToString(), meta.name});
            }
        };
        switch (c.kind) {
            case WatchChange::Kind::Added:
                registerFromVfs(c.path);
                break;
            case WatchChange::Kind::Removed:
                if (auto existing = db_.FindByPath(prefix + c.path); existing) {
                    (void)db_.Remove(existing->uuid);
                    indexer_.Remove(existing->uuid);
                    (void)EventBus::Instance().Publish(
                        events::ContentAssetRemoved{existing->uuid.ToString()});
                }
                break;
            case WatchChange::Kind::Moved:
                if (auto existing = db_.FindByPath(prefix + c.path); existing) {
                    // The file was renamed; update the record to the new path.
                    auto updated = *existing;
                    updated.path = c.toPath;
                    if (auto r = db_.Upsert(updated); r.ok()) indexer_.Index(updated);
                }
                registerFromVfs(c.toPath);
                break;
            case WatchChange::Kind::Changed:
                if (auto existing = db_.FindByPath(prefix + c.path); existing) {
                    (void)EventBus::Instance().Publish(events::ContentAssetChanged{
                        existing->uuid.ToString(), existing->name});
                }
                break;
        }
    });
    std::string relDir(dir);   // mount-relative dir; the list callback uses this
    auto r = watcher_.AddRoot(prefix + std::string(dir),
                              [vfs, relDir](std::string_view) -> std::optional<std::vector<std::string>> {
                                  auto list = vfs->List(relDir);
                                  if (!list.ok()) return std::nullopt;
                                  return list.value();
                              });
    if (r.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        activeWatchDir_ = prefix + std::string(dir);
    }
    return r;
}

void ContentManager::StopWatch() {
    watcher_.RemoveRoot(activeWatchDir_);
}

void ContentManager::PollWatch() {
    if (!started_.load()) return;
    (void)watcher_.Poll();
}

// ---------------------------------------------------------------------------
// Validation + persistence
// ---------------------------------------------------------------------------

ValidationReport ContentManager::ValidateAll() {
    auto library = db_.All();
    ValidationReport report = validator_.ValidateLibrary(library);
    auto vfs = FindMount("lib");
    for (const auto& m : library) {
        auto issues = validator_.Validate(
            m,
            [vfs](const std::string& path) -> std::optional<bool> {
                if (!vfs) return std::nullopt;
                auto e = vfs->Exists(path);
                return e.ok() ? std::optional<bool>{e.value()} : std::nullopt;
            },
            [vfs](const std::string& path) -> std::optional<uint64_t> {
                if (!vfs) return std::nullopt;
                auto s = vfs->Size(path);
                return s.ok() ? std::optional<uint64_t>{s.value()} : std::nullopt;
            });
        for (const auto& issue : issues.issues) {
            report.issues.push_back(issue);
            (void)EventBus::Instance().Publish(events::ContentValidationFailed{
                m.uuid.ToString(), issue.code + ": " + issue.message});
        }
    }
    return report;
}

Result<void> ContentManager::PersistLibrary() {
    auto& db = DatabaseManager::Instance();
    auto library = db_.All();
    for (const auto& m : library) {
        if (auto r = db.Put("content", m.uuid.ToString(),
                            AssetSerializer::ToJson(m)); !r.ok())
            return r;
    }
    // Tombstone stale rows.
    auto keys = db.Keys("content");
    for (const auto& key : keys) {
        Uuid u = Uuid::FromString(key);
        if (!db_.Contains(u)) (void)db.Remove("content", key);
    }
    return Ok();
}

Result<void> ContentManager::LoadLibrary() {
    auto& db = DatabaseManager::Instance();
    auto keys = db.Keys("content");
    std::vector<AssetMetadata> loaded;
    for (const auto& key : keys) {
        auto doc = db.Get("content", key);
        if (!doc.ok()) continue;
        auto m = AssetSerializer::FromJson(doc.value());
        if (!m.ok()) continue;
        loaded.push_back(m.value());
    }
    for (auto& m : loaded) {
        if (auto r = db_.Upsert(m); !r.ok()) return r;
        indexer_.Index(m);
    }
    if (!loaded.empty())
        Logger::Instance().Info(std::format("CAMS: restored {} assets from database",
                                            loaded.size()),
                                "CAMS");
    return Ok();
}

void ContentManager::OnPressureHigh() {
    // ResourceManager (10) says memory is tight: drop evictable cache bytes,
    // clear thumbnail cache, and defer future indexing work (index rebuilt on
    // next Reload/ReindexAll).
    size_t freed = ShrinkCache(0.5);
    size_t thumbs = thumbnails_.CacheCount();
    thumbnails_.ClearCache();
    Logger::Instance().Info(std::format("CAMS: memory pressure — freed {} cache bytes, "
                                         "dropped {} thumbnails",
                                         freed, thumbs),
                            "CAMS");
    (void)EventBus::Instance().Publish(events::ContentCacheUpdated{
        assets_.CachedCount(), assets_.GetCacheStats().bytes});
}

} // namespace bps::content
