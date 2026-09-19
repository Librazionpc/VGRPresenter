#include "modules/project/WorkspaceManager.hpp"

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

WorkspaceManager& WorkspaceManager::Instance() {
    static WorkspaceManager instance;
    return instance;
}

void WorkspaceManager::SetOpenDocuments(const std::vector<std::string>& docIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    openDocuments_ = docIds;
}

std::vector<std::string> WorkspaceManager::OpenDocuments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return openDocuments_;
}

void WorkspaceManager::AddOpenDocument(std::string_view docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(openDocuments_.begin(), openDocuments_.end(), docId) == openDocuments_.end())
        openDocuments_.push_back(std::string(docId));
}

void WorkspaceManager::RemoveOpenDocument(std::string_view docId) {
    std::lock_guard<std::mutex> lock(mutex_);
    openDocuments_.erase(std::remove(openDocuments_.begin(), openDocuments_.end(), docId),
                         openDocuments_.end());
}

void WorkspaceManager::SetSelectedDisplays(const std::vector<std::string>& displayIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    selectedDisplays_ = displayIds;
}

std::vector<std::string> WorkspaceManager::SelectedDisplays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return selectedDisplays_;
}

void WorkspaceManager::SetCurrentPresentation(std::string_view assetUuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentPresentation_ = std::string(assetUuid);
}

std::string WorkspaceManager::CurrentPresentation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentPresentation_;
}

void WorkspaceManager::SetCurrentTheme(std::string_view themeId) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentTheme_ = std::string(themeId);
}

std::string WorkspaceManager::CurrentTheme() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentTheme_;
}

void WorkspaceManager::SetZoom(double zoom) {
    std::lock_guard<std::mutex> lock(mutex_);
    zoom_ = zoom;
}

double WorkspaceManager::Zoom() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return zoom_;
}

void WorkspaceManager::AddRecentlyUsed(std::string_view kind, std::string_view id,
                                       std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                 [&](const Recent& r) { return r.kind == kind && r.id == id; }),
                  recent_.end());
    recent_.insert(recent_.begin(), Recent{std::string(kind), std::string(id),
                                           std::string(name), MsNow()});
    while (recent_.size() > static_cast<size_t>(recentLimit_)) recent_.pop_back();
}

std::vector<std::string> WorkspaceManager::RecentlyUsed(std::string_view kind, int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& r : recent_) {
        if (r.kind != kind) continue;
        out.push_back(r.name.empty() ? r.id : r.name);
        if (static_cast<int>(out.size()) >= limit) break;
    }
    return out;
}

json::Value WorkspaceManager::ToJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value::Object o;
    json::Value::Array docs;
    for (const auto& d : openDocuments_) docs.push_back(json::Value::String(d));
    o["openDocuments"] = json::Value(std::move(docs));
    json::Value::Array disp;
    for (const auto& d : selectedDisplays_) disp.push_back(json::Value::String(d));
    o["selectedDisplays"] = json::Value(std::move(disp));
    o["currentPresentation"] = json::Value::String(currentPresentation_);
    o["currentTheme"] = json::Value::String(currentTheme_);
    o["zoom"] = json::Value::Number(zoom_);
    json::Value::Array rec;
    for (const auto& r : recent_) {
        json::Value::Object e;
        e["kind"] = json::Value::String(r.kind);
        e["id"] = json::Value::String(r.id);
        e["name"] = json::Value::String(r.name);
        e["ts"] = json::Value::Number(static_cast<double>(r.ts));
        rec.push_back(json::Value(std::move(e)));
    }
    o["recent"] = json::Value(std::move(rec));
    return json::Value(std::move(o));
}

Result<void> WorkspaceManager::FromJson(const json::Value& v) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto* arr = v.Find("openDocuments"))
        if (const auto* a = arr->asArray())
            for (const auto& e : *a) openDocuments_.push_back(std::string(e.asString()));
    if (const auto* arr = v.Find("selectedDisplays"))
        if (const auto* a = arr->asArray())
            for (const auto& e : *a) selectedDisplays_.push_back(std::string(e.asString()));
    if (const auto* n = v.Find("currentPresentation")) currentPresentation_ = std::string(n->asString());
    if (const auto* n = v.Find("currentTheme")) currentTheme_ = std::string(n->asString());
    if (const auto* n = v.Find("zoom")) zoom_ = n->asNumber(1.0);
    if (const auto* arr = v.Find("recent")) {
        if (const auto* a = arr->asArray()) {
            for (const auto& e : *a) {
                Recent r;
                if (const auto* n = e.Find("kind")) r.kind = std::string(n->asString());
                if (const auto* n = e.Find("id")) r.id = std::string(n->asString());
                if (const auto* n = e.Find("name")) r.name = std::string(n->asString());
                if (const auto* n = e.Find("ts")) r.ts = n->asInt(0);
                recent_.push_back(r);
            }
        }
    }
    return Ok();
}

Result<void> WorkspaceManager::Save() {
    return DatabaseManager::Instance().Put("workspace", "default", ToJson());
}

Result<void> WorkspaceManager::Load() {
    auto doc = DatabaseManager::Instance().Get("workspace", "default");
    if (!doc.ok()) return Ok();   // no saved workspace yet
    {
        std::lock_guard<std::mutex> lock(mutex_);
        openDocuments_.clear();
        selectedDisplays_.clear();
        recent_.clear();
    }
    return FromJson(doc.value());   // FromJson takes the lock itself
}

} // namespace bps::project
