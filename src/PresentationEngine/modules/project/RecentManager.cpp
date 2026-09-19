#include "modules/project/RecentManager.hpp"

#include "core/database/DatabaseManager.hpp"

#include <algorithm>
#include <chrono>

namespace bps::project {

namespace {
int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

RecentManager& RecentManager::Instance() {
    static RecentManager instance;
    return instance;
}

Result<void> RecentManager::Record(std::string_view kind, std::string_view id,
                                   std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const RecentItem& r) {
                                    return r.kind == kind && r.id == id;
                                }),
                 items_.end());
    items_.insert(items_.begin(),
                  RecentItem{std::string(kind), std::string(id), std::string(name), MsNow()});
    while (items_.size() > limit_) items_.pop_back();
    return Ok();
}

std::vector<RecentItem> RecentManager::List(std::string_view kind, int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecentItem> out;
    for (const auto& r : items_) {
        if (r.kind != kind) continue;
        out.push_back(r);
        if (static_cast<int>(out.size()) >= limit) break;
    }
    return out;
}

Result<void> RecentManager::Remove(std::string_view kind, std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const RecentItem& r) {
                                    return r.kind == kind && r.id == id;
                                }),
                 items_.end());
    return Ok();
}

Result<void> RecentManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    return Ok();
}

Result<void> RecentManager::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value::Array arr;
    for (const auto& r : items_) {
        json::Value::Object o;
        o["kind"] = json::Value::String(r.kind);
        o["id"] = json::Value::String(r.id);
        o["name"] = json::Value::String(r.name);
        o["ts"] = json::Value::Number(static_cast<double>(r.timestampMs));
        arr.push_back(json::Value(std::move(o)));
    }
    return DatabaseManager::Instance().Put("recent", "items", json::Value(std::move(arr)));
}

Result<void> RecentManager::Load() {
    auto doc = DatabaseManager::Instance().Get("recent", "items");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    // The stored doc is a bare JSON array of recent items.
    if (doc.value().type() == json::Value::Type::Array) {
        for (const auto& e : *doc.value().asArray()) {
            RecentItem r;
            if (const auto* n = e.Find("kind")) r.kind = std::string(n->asString());
            if (const auto* n = e.Find("id")) r.id = std::string(n->asString());
            if (const auto* n = e.Find("name")) r.name = std::string(n->asString());
            if (const auto* n = e.Find("ts")) r.timestampMs = n->asInt(0);
            items_.push_back(r);
        }
    }
    return Ok();
}

size_t RecentManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

} // namespace bps::project
