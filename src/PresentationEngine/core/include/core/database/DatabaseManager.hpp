#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps {

// Minimal embedded document store (core/database/). The core persists small
// structured data (settings, workspace state, plugin state) as JSON documents
// keyed by collection + key. Feature modules needing a full SQL/vector engine
// plug their own backend behind IService instead of living in the core.
class DatabaseManager final : public IService {
public:
    static DatabaseManager& Instance();

    Result<void> Initialize() override { return Open({}); }
    Result<void> Open(std::string filePath);   // empty path = in-memory only
    Result<void> Shutdown() override;          // flushes to disk when a file is set

    Result<void> Put(std::string_view collection, std::string_view key, json::Value doc);
    Result<json::Value> Get(std::string_view collection, std::string_view key) const;
    Result<void> Remove(std::string_view collection, std::string_view key);
    std::vector<std::string> Keys(std::string_view collection) const;
    std::vector<std::string> Collections() const;
    Result<void> Flush();
    size_t DocumentCount() const;

    const char* ServiceName() const noexcept override { return "DatabaseManager"; }
    HealthReport GetHealth() const override;

private:
    DatabaseManager() = default;
    // Serialize the in-memory documents to JSON text. Requires mutex_ held.
    std::string SerializeLocked() const;
    // Write JSON text through the PAL filesystem. No lock is held (callers
    // hold writeMutex_ so snapshots stay ordered on disk); the disk write must
    // never run under mutex_, which every other DB op also needs.
    static Result<void> WriteFile(const std::string& filePath, const std::string& text);

    mutable std::mutex mutex_;
    std::mutex writeMutex_;                      // serializes disk writes (outside mutex_)
    std::unordered_map<std::string, std::unordered_map<std::string, json::Value>> db_;
    std::string filePath_;                       // guarded by mutex_
    std::atomic<bool> initialized_{false};
};

} // namespace bps
