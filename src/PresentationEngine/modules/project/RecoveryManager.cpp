#include "modules/project/RecoveryManager.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace bps::project {

namespace {
int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
json::Value ItemToJson(const RecoveryItem& i) {
    json::Value::Object o;
    o["id"] = json::Value::String(i.kind);   // id = kind+timestamp derived on load
    o["kind"] = json::Value::String(i.kind);
    o["projectId"] = json::Value::String(i.projectId);
    o["documentId"] = json::Value::String(i.documentId);
    o["detail"] = json::Value::String(i.detail);
    o["ts"] = json::Value::Number(static_cast<double>(i.timestampMs));
    return json::Value(std::move(o));
}
} // namespace

RecoveryManager& RecoveryManager::Instance() {
    static RecoveryManager instance;
    return instance;
}

Result<void> RecoveryManager::RecordUnsavedWork(std::string_view projectId,
                                                std::string_view documentId,
                                                std::string_view detail) {
    RecoveryItem item;
    item.kind = "unsaved-work";
    item.projectId = std::string(projectId);
    item.documentId = std::string(documentId);
    item.detail = std::string(detail);
    item.timestampMs = MsNow();

    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const RecoveryItem& x) {
                                    return x.projectId == projectId &&
                                           x.documentId == documentId && x.kind == item.kind;
                                }),
                 items_.end());
    items_.push_back(item);
    return Persist();
}

Result<void> RecoveryManager::ClearProject(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const RecoveryItem& x) { return x.projectId == projectId; }),
                 items_.end());
    return Persist();
}

Result<void> RecoveryManager::Resolve(std::string_view itemId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(items_.begin(), items_.end(),
                           [&](const RecoveryItem& x) {
                               return x.kind + ":" + x.projectId + ":" + x.documentId == itemId;
                           });
    if (it == items_.end()) return Ok();   // already resolved
    items_.erase(it);
    return Persist();
}

bool RecoveryManager::DetectCrashedSession() {
    auto prev = SessionManager::Instance().Previous();
    if (!prev.ok()) return false;
    if (prev.value().cleanShutdown) return false;

    RecoveryItem item;
    item.kind = "crashed-session";
    item.projectId = prev.value().activeProjectId;
    item.detail =
        std::format("Previous session ended unexpectedly (started {})",
                    prev.value().startedAtMs);
    item.timestampMs = MsNow();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool hasCrash = false;
        for (const auto& x : items_)
            if (x.kind == "crashed-session") hasCrash = true;
        if (!hasCrash) {
            items_.push_back(item);
            (void)Persist();
        }
    }
    // Publish outside the lock: subscribers may call back into the engine.
    Logger::Instance().Warning("Crash recovery: unsaved work detected", "RecoveryManager");
    (void)EventBus::Instance().Publish(events::RecoveryAvailable{prev.value().activeProjectId,
                                                                 item.detail});
    return true;
}

Result<std::string> RecoveryManager::CurrentSessionId() const {
    return Result<std::string>{SessionManager::Instance().Info().id};
}

std::vector<RecoveryItem> RecoveryManager::Suggestions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

Result<RecoveryItem> RecoveryManager::Suggest(std::string_view projectId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& x : items_)
        if (x.projectId == projectId) return Result<RecoveryItem>{x};
    return Error::Make(Err::Project_RecoveryNotFound, "Recovery", "no recovery for project");
}

size_t RecoveryManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

Result<void> RecoveryManager::Persist() {   // requires mutex_ held by the caller
    json::Value::Array arr;
    for (const auto& i : items_) arr.push_back(ItemToJson(i));
    return DatabaseManager::Instance().Put("recovery", "items", json::Value(std::move(arr)));
}

} // namespace bps::project
