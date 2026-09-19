#include "core/database/DatabaseManager.hpp"

#include "platform/PlatformAccessor.hpp"
#include <format>

namespace bps {

DatabaseManager& DatabaseManager::Instance() {
    static DatabaseManager instance;
    return instance;
}

Result<void> DatabaseManager::Open(std::string filePath) {
    std::lock_guard<std::mutex> lock(mutex_);
    db_.clear();
    initialized_.store(true);
    if (filePath.empty()) {
        filePath_.clear();
        return Ok();   // in-memory only
    }

    // Load first; the path is adopted only when the file is valid, so a later
    // Shutdown()->Flush() can never overwrite a corrupt database with an empty one.
    auto read = platform::PlatformAccessor::Get().Filesystem().ReadText(filePath);
    if (!read.ok()) {
        filePath_ = std::move(filePath);   // first run: nothing to load
        return Ok();
    }
    const std::string& text = read.value();
    if (text.empty()) {
        filePath_ = std::move(filePath);
        return Ok();
    }

    auto parsed = json::Parse(text);
    if (!parsed.ok()) {
        filePath_.clear();   // do not enable persistence over a corrupt file
        return Error::Make(Err::Config_ParseFailed, "DatabaseManager",
                           "corrupt database file: " + parsed.error().message);
    }
    const json::Value* collections = parsed.value().Find("collections");
    if (!collections || !collections->asObject()) {
        filePath_ = std::move(filePath);
        return Ok();
    }
    for (const auto& [coll, docs] : *collections->asObject()) {
        const json::Value::Object* obj = docs.asObject();
        if (!obj) continue;
        for (const auto& [key, doc] : *obj) db_[coll][key] = doc;
    }
    filePath_ = std::move(filePath);
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

Result<void> DatabaseManager::Persist() {
    if (filePath_.empty()) return Ok();
    json::Value::Object collections;
    for (const auto& [coll, docs] : db_) {
        json::Value::Object o;
        for (const auto& [key, doc] : docs) o[key] = doc;
        collections[coll] = json::Value(std::move(o));
    }
    json::Value::Object root;
    root["collections"] = json::Value(std::move(collections));
    auto& fsys = platform::PlatformAccessor::Get().Filesystem();
    size_t slash = filePath_.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        if (auto r = fsys.CreateDirectories(filePath_.substr(0, slash)); !r.ok()) return r;
    }
    auto w = fsys.Write(filePath_, json::Value(std::move(root)).ToString());
    if (!w.ok())
        return Error::Make(Err::IoError, "DatabaseManager",
                           "cannot write database file " + filePath_ + ": " + w.error().message);
    return Ok();
}

Result<void> DatabaseManager::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    return Persist();
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
