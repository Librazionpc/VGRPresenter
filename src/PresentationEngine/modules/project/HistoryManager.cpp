#include "modules/project/HistoryManager.hpp"

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
json::Value EntryToJson(const HistoryManager::Entry& e) {
    json::Value::Object o;
    o["projectId"] = json::Value::String(e.projectId);
    o["command"] = json::Value::String(e.command);
    o["detail"] = json::Value::String(e.detail);
    o["ts"] = json::Value::Number(static_cast<double>(e.timestampMs));
    return json::Value(std::move(o));
}
HistoryManager::Entry EntryFromJson(const json::Value& v) {
    HistoryManager::Entry e;
    if (const auto* n = v.Find("projectId")) e.projectId = std::string(n->asString());
    if (const auto* n = v.Find("command")) e.command = std::string(n->asString());
    if (const auto* n = v.Find("detail")) e.detail = std::string(n->asString());
    if (const auto* n = v.Find("ts")) e.timestampMs = n->asInt(0);
    return e;
}
} // namespace

HistoryManager& HistoryManager::Instance() {
    static HistoryManager instance;
    return instance;
}

Result<void> HistoryManager::Record(std::string_view projectId, std::string_view command,
                                    std::string_view detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(Entry{std::string(projectId), std::string(command),
                             std::string(detail), MsNow()});
    while (entries_.size() > limit_) entries_.erase(entries_.begin());
    return Ok();
}

std::vector<HistoryManager::Entry> HistoryManager::EditHistory(std::string_view projectId,
                                                               int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> out;
    for (auto it = entries_.rbegin(); it != entries_.rend() && static_cast<int>(out.size()) < limit;
         ++it)
        if (it->projectId == projectId) out.push_back(*it);
    return out;
}

Result<void> HistoryManager::ClearEditHistory(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.projectId == projectId; }),
                   entries_.end());
    return Ok();
}

void HistoryManager::RecordSave(std::string_view projectId, std::string_view path,
                                bool autosave) {
    std::lock_guard<std::mutex> lock(mutex_);
    saves_.push_back(Entry{std::string(projectId), autosave ? "autosave" : "save",
                           std::string(path), MsNow()});
    while (saves_.size() > limit_) saves_.erase(saves_.begin());
}

std::vector<HistoryManager::Entry> HistoryManager::SaveHistory(std::string_view projectId,
                                                               int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> out;
    for (auto it = saves_.rbegin(); it != saves_.rend() && static_cast<int>(out.size()) < limit;
         ++it)
        if (it->projectId == projectId) out.push_back(*it);
    return out;
}

Result<void> HistoryManager::Checkpoint(std::string_view projectId, std::string_view detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoints_.push_back(Entry{std::string(projectId), "checkpoint",
                                 std::string(detail), MsNow()});
    while (checkpoints_.size() > limit_) checkpoints_.erase(checkpoints_.begin());
    return Ok();
}

std::vector<HistoryManager::Entry> HistoryManager::Checkpoints(std::string_view projectId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> out;
    for (auto it = checkpoints_.rbegin(); it != checkpoints_.rend(); ++it)
        if (it->projectId == projectId) out.push_back(*it);
    return out;
}

Result<void> HistoryManager::ClearCheckpoints(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoints_.erase(std::remove_if(checkpoints_.begin(), checkpoints_.end(),
                                      [&](const Entry& e) { return e.projectId == projectId; }),
                       checkpoints_.end());
    return Ok();
}

Result<void> HistoryManager::Save() {
    json::Value::Object o;
    json::Value::Array eArr, sArr, cArr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& e : entries_) eArr.push_back(EntryToJson(e));
        for (const auto& s : saves_) sArr.push_back(EntryToJson(s));
        for (const auto& c : checkpoints_) cArr.push_back(EntryToJson(c));
    }
    o["entries"] = json::Value(std::move(eArr));
    o["saves"] = json::Value(std::move(sArr));
    o["checkpoints"] = json::Value(std::move(cArr));
    return DatabaseManager::Instance().Put("history", "default", json::Value(std::move(o)));
}

Result<void> HistoryManager::Load() {
    auto doc = DatabaseManager::Instance().Get("history", "default");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    saves_.clear();
    checkpoints_.clear();
    if (const auto* arr = doc.value().Find("entries"))
        if (const auto* a = arr->asArray())
            for (const auto& v : *a) entries_.push_back(EntryFromJson(v));
    if (const auto* arr = doc.value().Find("saves"))
        if (const auto* a = arr->asArray())
            for (const auto& v : *a) saves_.push_back(EntryFromJson(v));
    if (const auto* arr = doc.value().Find("checkpoints"))
        if (const auto* a = arr->asArray())
            for (const auto& v : *a) checkpoints_.push_back(EntryFromJson(v));
    return Ok();
}

Result<void> HistoryManager::ClearAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    saves_.clear();
    checkpoints_.clear();
    return Ok();
}

size_t HistoryManager::TotalEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size() + saves_.size() + checkpoints_.size();
}

} // namespace bps::project
