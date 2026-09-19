#include "modules/project/FavoritesManager.hpp"

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

FavoritesManager& FavoritesManager::Instance() {
    static FavoritesManager instance;
    return instance;
}

Result<void> FavoritesManager::Add(std::string_view kind, std::string_view id,
                                   std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& f : items_)
        if (f.kind == kind && f.id == id) return Ok();   // already a favorite
    items_.push_back(FavoriteItem{std::string(kind), std::string(id), std::string(name), MsNow()});
    return Ok();
}

Result<void> FavoritesManager::Remove(std::string_view kind, std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const FavoriteItem& f) {
                                    return f.kind == kind && f.id == id;
                                }),
                 items_.end());
    return Ok();
}

bool FavoritesManager::IsFavorite(std::string_view kind, std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& f : items_)
        if (f.kind == kind && f.id == id) return true;
    return false;
}

std::vector<FavoriteItem> FavoritesManager::List(std::string_view kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FavoriteItem> out;
    for (const auto& f : items_)
        if (kind.empty() || f.kind == kind) out.push_back(f);
    return out;
}

Result<void> FavoritesManager::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value::Array arr;
    for (const auto& f : items_) {
        json::Value::Object o;
        o["kind"] = json::Value::String(f.kind);
        o["id"] = json::Value::String(f.id);
        o["name"] = json::Value::String(f.name);
        o["addedAtMs"] = json::Value::Number(static_cast<double>(f.addedAtMs));
        arr.push_back(json::Value(std::move(o)));
    }
    return DatabaseManager::Instance().Put("favorites", "items", json::Value(std::move(arr)));
}

Result<void> FavoritesManager::Load() {
    auto doc = DatabaseManager::Instance().Get("favorites", "items");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    if (doc.value().type() != json::Value::Type::Array) return Ok();
    for (const auto& e : *doc.value().asArray()) {
        FavoriteItem f;
        if (const auto* n = e.Find("kind")) f.kind = std::string(n->asString());
        if (const auto* n = e.Find("id")) f.id = std::string(n->asString());
        if (const auto* n = e.Find("name")) f.name = std::string(n->asString());
        if (const auto* n = e.Find("addedAtMs")) f.addedAtMs = n->asInt(0);
        items_.push_back(f);
    }
    return Ok();
}

size_t FavoritesManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

} // namespace bps::project
