#include "core/database/DatabaseManager.hpp"

#include "core/recovery/DocumentRecovery.hpp"
#include "platform/PlatformAccessor.hpp"
#include <format>

namespace bps {
DatabaseManager& DatabaseManager::Instance() {
    static DatabaseManager instance;
    return instance;
}

Result<void> DatabaseManager::Open(std::string filePath) {
    std::string text;
    if (!filePath.empty()) {
        auto read = platform::PlatformAccessor::Get().Filesystem().ReadText(filePath);
        if (read.ok())
            text = read.value();
        // Unreadable (locked by another instance, permissions) is still the
        // first-run shape: nothing to load, nothing to quarantine.
    }

    bool corrupt = false;
    std::string parseError;
    json::Value root;   // Null unless a document was parsed
    if (!text.empty()) {
        auto parsed = json::Parse(text);
        if (parsed.ok()) {
            root = std::move(parsed.value());
        } else {
            corrupt = true;
            parseError = parsed.error().message;
        }
    }

    if (corrupt)
        (void)recovery::SetAsideDamagedDocument(filePath, "DatabaseManager", parseError);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        db_.clear();
        initialized_.store(true);
        if (filePath.empty()) {
            filePath_.clear();
            return Ok();   // in-memory only
        }

        filePath_ = std::move(filePath);
        if (!corrupt && !text.empty()) {
            const json::Value* collections = root.Find("collections");
            if (collections && collections->asObject()) {
                for (const auto& [coll, docs] : *collections->asObject()) {
                    const json::Value::Object* obj = docs.asObject();
                    if (!obj) continue;
                    for (const auto& [key, doc] : *obj) db_[coll][key] = doc;
                }
            }
        }
    }

    return Ok();
}

Result<void> DatabaseManager::Put(std::string_view collection, std::string_view key,
                                  json::Value doc) {
    if (collection.empty() || key.empty())
        return Error::Make(Err::InvalidArgument, "DatabaseManager",
                           "collection and key are required");
    std::lock_guard<std::mutex> lock(mutex_);
    db_[std::string(collection)][std::string(key)] = std::move(doc);
    return Ok();
}

Result<json::Value> DatabaseManager::Get(std::string_view collection,
                                         std::string_view key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto c = db_.find(std::string(collection));
    if (c == db_.end())
        return Error::Make(Err::NotFound, "DatabaseManager", "unknown collection");
    auto it = c->second.find(std::string(key));
    if (it == c->second.end())
        return Error::Make(Err::NotFound, "DatabaseManager", "unknown key");
    return it->second;
}

Result<void> DatabaseManager::Remove(std::string_view collection, std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto c = db_.find(std::string(collection));
    if (c == db_.end())
        return Error::Make(Err::NotFound, "DatabaseManager", "unknown collection");
    if (!c->second.erase(std::string(key)))
        return Error::Make(Err::NotFound, "DatabaseManager", "unknown key");
    return Ok();
}

std::vector<std::string> DatabaseManager::Keys(std::string_view collection) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    auto c = db_.find(std::string(collection));
    if (c == db_.end()) return out;
    for (const auto& [k, v] : c->second) {
        (void)v;
        out.push_back(k);
    }
    return out;
}

std::vector<std::string> DatabaseManager::Collections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [c, docs] : db_) {
        (void)docs;
        out.push_back(c);
    }
    return out;
}

std::string DatabaseManager::SerializeLocked() const {
    json::Value::Object collections;
    for (const auto& [coll, docs] : db_) {
        json::Value::Object o;
        for (const auto& [key, doc] : docs) o[key] = doc;
        collections[coll] = json::Value(std::move(o));
    }
    json::Value::Object root;
    root["collections"] = json::Value(std::move(collections));
    return json::Value(std::move(root)).ToString();
}

Result<void> DatabaseManager::WriteFile(const std::string& filePath, const std::string& text) {
    auto& fsys = platform::PlatformAccessor::Get().Filesystem();
    auto w = fsys.WriteAtomic(filePath, text);
    if (!w.ok())
        return Error::Make(Err::IoError, "DatabaseManager",
                           "cannot write database file " + filePath + ": " + w.error().message);
    return Ok();
}

Result<void> DatabaseManager::Flush() {
    std::lock_guard<std::mutex> writeLock(writeMutex_);
    std::string path;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (filePath_.empty()) return Ok();
        path = filePath_;
        text = SerializeLocked();
    }
    return WriteFile(path, text);
}

Result<void> DatabaseManager::Shutdown() {
    if (!initialized_.exchange(false)) return Ok();
    return Flush();
}

size_t DatabaseManager::DocumentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [coll, docs] : db_) {
        (void)coll;
        n += docs.size();
    }
    return n;
}

HealthReport DatabaseManager::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t docs = 0;
    for (const auto& [coll, d] : db_) {
        (void)coll;
        docs += d.size();
    }
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("collections={} documents={}{}", db_.size(), docs,
                           filePath_.empty() ? " [in-memory]" : " file=" + filePath_);
    return r;
}
} // namespace bps