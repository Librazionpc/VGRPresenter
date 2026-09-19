#include "modules/project/SessionManager.hpp"

#include "core/database/DatabaseManager.hpp"

#include <chrono>
#include <format>

namespace bps::project {

namespace {
int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string GenerateId() {
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t v = static_cast<uint64_t>(t) ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 5);
    return std::format("sess-{:x}", v);
}
} // namespace

SessionManager& SessionManager::Instance() {
    static SessionManager instance;
    return instance;
}

Result<void> SessionManager::Begin(std::string_view user) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.id = GenerateId();
        info_.user = std::string(user);
        info_.startedAtMs = MsNow();
        info_.lastActiveAtMs = info_.startedAtMs;
        info_.cleanShutdown = false;
        info_.activeProjectId.clear();
        presentationsOpened_.store(0);
        savesPerformed_.store(0);
        errorsEncountered_.store(0);
    }
    return Save();   // Save() takes the lock itself — never call under mutex_
}

Result<void> SessionManager::End() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.lastActiveAtMs = MsNow();
        info_.cleanShutdown = true;
    }
    cleanShutdown_.store(true);
    return Save();   // Save() takes the lock itself — never call under mutex_
}

void SessionManager::SetActiveProject(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    info_.activeProjectId = std::string(projectId);
    info_.lastActiveAtMs = MsNow();
}

std::string SessionManager::ActiveProject() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_.activeProjectId;
}

void SessionManager::SetUser(std::string_view user) {
    std::lock_guard<std::mutex> lock(mutex_);
    info_.user = std::string(user);
}

std::string SessionManager::User() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_.user;
}

Result<void> SessionManager::Save() {
    json::Value::Object o;
    std::lock_guard<std::mutex> lock(mutex_);
    info_.presentationsOpened = presentationsOpened_.load();
    info_.savesPerformed = savesPerformed_.load();
    info_.errorsEncountered = errorsEncountered_.load();
    o["id"] = json::Value::String(info_.id);
    o["user"] = json::Value::String(info_.user);
    o["activeProjectId"] = json::Value::String(info_.activeProjectId);
    o["startedAtMs"] = json::Value::Number(static_cast<double>(info_.startedAtMs));
    o["lastActiveAtMs"] = json::Value::Number(static_cast<double>(info_.lastActiveAtMs));
    o["cleanShutdown"] = json::Value::Bool(info_.cleanShutdown);
    o["presentationsOpened"] = json::Value::Number(static_cast<double>(info_.presentationsOpened));
    o["savesPerformed"] = json::Value::Number(static_cast<double>(info_.savesPerformed));
    o["errorsEncountered"] = json::Value::Number(static_cast<double>(info_.errorsEncountered));
    return DatabaseManager::Instance().Put("session", "current", json::Value(std::move(o)));
}

Result<void> SessionManager::Load() {
    auto doc = DatabaseManager::Instance().Get("session", "current");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    const json::Value& v = doc.value();
    if (const auto* n = v.Find("id")) info_.id = std::string(n->asString());
    if (const auto* n = v.Find("user")) info_.user = std::string(n->asString());
    if (const auto* n = v.Find("activeProjectId")) info_.activeProjectId = std::string(n->asString());
    if (const auto* n = v.Find("startedAtMs")) info_.startedAtMs = n->asInt(0);
    if (const auto* n = v.Find("lastActiveAtMs")) info_.lastActiveAtMs = n->asInt(0);
    if (const auto* n = v.Find("cleanShutdown")) info_.cleanShutdown = n->asBool(true);
    if (const auto* n = v.Find("presentationsOpened")) info_.presentationsOpened = n->asInt(0);
    if (const auto* n = v.Find("savesPerformed")) info_.savesPerformed = n->asInt(0);
    if (const auto* n = v.Find("errorsEncountered")) info_.errorsEncountered = n->asInt(0);
    cleanShutdown_.store(info_.cleanShutdown);
    presentationsOpened_.store(info_.presentationsOpened);
    savesPerformed_.store(info_.savesPerformed);
    errorsEncountered_.store(info_.errorsEncountered);
    return Ok();
}

Result<SessionInfo> SessionManager::Previous() const {
    auto doc = DatabaseManager::Instance().Get("session", "current");
    if (!doc.ok())
        return Error::Make(Err::Project_RecoveryNotFound, "Session", "no previous session");
    SessionInfo s;
    const json::Value& v = doc.value();
    if (const auto* n = v.Find("id")) s.id = std::string(n->asString());
    if (const auto* n = v.Find("user")) s.user = std::string(n->asString());
    if (const auto* n = v.Find("activeProjectId")) s.activeProjectId = std::string(n->asString());
    if (const auto* n = v.Find("cleanShutdown")) s.cleanShutdown = n->asBool(true);
    if (const auto* n = v.Find("startedAtMs")) s.startedAtMs = n->asInt(0);
    if (const auto* n = v.Find("savesPerformed")) s.savesPerformed = n->asInt(0);
    if (const auto* n = v.Find("errorsEncountered")) s.errorsEncountered = n->asInt(0);
    return Result<SessionInfo>{s};
}

} // namespace bps::project
