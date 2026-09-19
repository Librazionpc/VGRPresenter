#pragma once

// ContentManager (docs/specs/13 §Content Manager): the ONLY public API for
// content. Every module uses this. Find/load/save/import/export/delete/move/
// rename/duplicate/search. Integrates the Core (Logger, Config, EventBus,
// ThreadPool, Scheduler, ResourceManager, DatabaseManager) and the PAL (all
// file I/O through the VFS). Implements the full IService lifecycle contract.

#include "modules/content/AssetCache.hpp"
#include "modules/content/AssetDatabase.hpp"
#include "modules/content/AssetIndexer.hpp"
#include "modules/content/AssetLoader.hpp"
#include "modules/content/AssetManager.hpp"
#include "modules/content/AssetValidator.hpp"
#include "modules/content/AssetWatcher.hpp"
#include "modules/content/IImporter.hpp"
#include "modules/content/IExporter.hpp"
#include "modules/content/ImportManager.hpp"
#include "modules/content/ThumbnailManager.hpp"
#include "modules/content/Vfs.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::content {

struct ImportOptions {
    std::string destinationDir;   // VFS dir inside the library mount
    std::string suggestedName;
    std::vector<std::string> tags;
    std::string category;
    std::string author;
    bool generateThumbnail = true;
};

class ContentManager final : public IService {
public:
    static ContentManager& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "ContentManager"; }

    // --- VFS mounts ---
    // Mount a filesystem-backed library root (creates the dir if missing).
    Result<void> MountDisk(std::string name, std::string hostDir);
    // Mount an in-memory library (tests, transient content).
    Result<std::shared_ptr<IVfs>> MountMemory(std::string name);
    // Mount a read-only ZIP archive as a library.
    Result<std::shared_ptr<IVfs>> MountZip(std::string name, std::string archivePath);
    Result<void> Unmount(std::string_view name);
    std::vector<std::string> MountNames() const;

    // --- Library: metadata ---
    Result<Uuid> ImportFile(std::string hostPath, const ImportOptions& opts = {});
    Result<Uuid> CreateText(std::string name, AssetType type, std::string text,
                            const ImportOptions& opts = {});
    Result<AssetMetadata> GetMetadata(const Uuid& uuid) const;
    std::vector<AssetMetadata> List() const;
    size_t AssetCount() const;
    std::optional<AssetMetadata> FindByPath(std::string_view path) const;

    // --- Runtime assets ---
    Result<std::shared_ptr<RuntimeAsset>> Load(const Uuid& uuid);
    Result<std::shared_ptr<RuntimeAsset>> Load(const AssetMetadata& meta);
    void LoadAsync(const Uuid& uuid, CancelToken cancel,
                   std::function<void(Result<std::shared_ptr<RuntimeAsset>>)> done);
    Result<void> Unload(const Uuid& uuid);
    Result<void> Pin(const Uuid& uuid, bool pinned);
    Result<void> Prefetch(const Uuid& uuid);
    std::shared_ptr<RuntimeAsset> Peek(const Uuid& uuid) const;
    size_t LoadedCount() const;

    // --- Content operations (all through the VFS) ---
    Result<std::string> ReadText(const Uuid& uuid);
    Result<std::vector<uint8_t>> ReadBytes(const Uuid& uuid);
    Result<void> Save(const Uuid& uuid, std::vector<uint8_t> bytes);
    Result<void> SaveText(const Uuid& uuid, std::string text);
    Result<void> Delete(const Uuid& uuid);
    Result<void> Move(const Uuid& uuid, std::string_view newPath);
    Result<void> Rename(const Uuid& uuid, std::string_view newName);
    Result<Uuid> Duplicate(const Uuid& uuid);
    Result<void> Export(const Uuid& uuid, std::string_view format, std::string hostDestPath);
    Result<void> ExportPackage(const std::vector<Uuid>& uuids, std::string hostDestPath);

    // --- Search (AssetIndexer) ---
    std::vector<SearchResult> Search(const SearchQuery& q) const;
    void ReindexAll();

    // --- Extension points ---
    Result<void> RegisterImporter(std::shared_ptr<IImporter> importer);
    Result<void> RegisterExporter(std::shared_ptr<IExporter> exporter);
    std::vector<std::string> SupportedImportExtensions() const;
    std::vector<std::string> SupportedExportFormats() const;

    // --- Cache control (ResourceManager integration) ---
    CacheStats GetCacheStats() const;
    size_t Evict();
    size_t ShrinkCache(double fraction);
    void SetCacheCapacity(size_t bytes);

    // --- Watcher ---
    Result<void> WatchRoot(std::string_view mountName, std::string_view dir);
    void StopWatch();
    void PollWatch();

    // --- Validation ---
    ValidationReport ValidateAll();

private:
    ContentManager() = default;

    std::shared_ptr<IVfs> FindMount(std::string_view name) const;
    std::string MountPrefix(std::string_view name) const;
    Result<std::vector<uint8_t>> ReadViaVfs(const AssetMetadata& meta) const;
    Result<void> PersistLibrary();
    Result<void> LoadLibrary();
    void OnPressureHigh();
    void ScanMountIntoLibrary(const std::string& mountName);

    std::atomic<bool> initialized_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint64_t> errorCount_{0};
    uint64_t heartbeatTask_ = 0;   // TaskScheduler id of the watcher heartbeat
    std::vector<Subscription> subscriptions_;   // EventBus tokens (released on Shutdown)

    AssetDatabase db_;
    AssetManager assets_;
    AssetIndexer indexer_;
    ImportManager importers_;
    ExportManager exporters_;
    AssetWatcher watcher_;
    ThumbnailManager thumbnails_;
    AssetValidator validator_;
    std::unique_ptr<AssetLoader> loader_;

    mutable std::mutex mountMutex_;
    std::unordered_map<std::string, std::shared_ptr<IVfs>> mounts_;

    mutable std::mutex mutex_;
    std::string lastError_;
    uint64_t operations_ = 0;
    std::chrono::microseconds opLatencySum_{0};
    std::string activeWatchDir_;
};

} // namespace bps::content
