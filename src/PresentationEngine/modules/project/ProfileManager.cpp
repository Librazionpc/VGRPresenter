#include "modules/project/ProfileManager.hpp"

#include "core/config/ConfigurationManager.hpp"
#include "core/database/DatabaseManager.hpp"

#include <algorithm>
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
    uint64_t v = static_cast<uint64_t>(t) ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 17);
    return std::format("prof-{:x}", v);
}
} // namespace

ProfileManager& ProfileManager::Instance() {
    static ProfileManager instance;
    return instance;
}

Result<std::string> ProfileManager::Create(std::string_view kind, std::string_view name,
                                           const json::Value& settings,
                                           std::string_view description) {
    if (kind.empty() || name.empty())
        return Error::Make(Err::InvalidArgument, "Profile", "kind and name required");
    Profile p;
    p.id = GenerateId();
    p.kind = std::string(kind);
    p.name = std::string(name);
    p.description = std::string(description);
    p.createdAtMs = MsNow();
    p.settings = settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        profiles_.push_back(p);
    }
    return Result<std::string>{p.id};
}

std::vector<Profile> ProfileManager::List(std::string_view kind) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Profile> out;
    for (const auto& p : profiles_)
        if (kind.empty() || p.kind == kind) out.push_back(p);
    return out;
}

Result<Profile> ProfileManager::Get(std::string_view profileId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](const Profile& p) { return p.id == profileId; });
    if (it == profiles_.end())
        return Error::Make(Err::NotFound, "Profile", "no such profile");
    return Result<Profile>{*it};
}

Result<void> ProfileManager::Remove(std::string_view profileId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](const Profile& p) { return p.id == profileId; });
    if (it == profiles_.end())
        return Error::Make(Err::NotFound, "Profile", "no such profile");
    profiles_.erase(it);
    return Ok();
}

Result<void> ProfileManager::UpdateSettings(std::string_view profileId,
                                            const json::Value& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(profiles_.begin(), profiles_.end(),
                           [&](const Profile& p) { return p.id == profileId; });
    if (it == profiles_.end())
        return Error::Make(Err::NotFound, "Profile", "no such profile");
    it->settings = settings;
    ++it->version;
    return Ok();
}

Result<void> ProfileManager::Apply(std::string_view profileId) {
    auto p = Get(profileId);
    if (!p.ok()) return p.error();
    auto& config = ConfigurationManager::Instance();
    // Flatten the settings object: top-level keys are dot-keys.
    if (p.value().settings.type() == json::Value::Type::Object) {
        for (const auto& [key, val] : *p.value().settings.asObject())
            (void)config.Set(key, val);
    }
    (void)DatabaseManager::Instance().Put("profiles", "active-" + p.value().kind,
                                          json::Value::String(p.value().id));
    return Ok();
}

Result<Profile> ProfileManager::Active(std::string_view kind) const {
    auto doc = DatabaseManager::Instance().Get("profiles", "active-" + std::string(kind));
    if (!doc.ok()) return Error::Make(Err::NotFound, "Profile", "no active profile");
    return Get(doc.value().asString());
}

Result<void> ProfileManager::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value::Array arr;
    for (const auto& p : profiles_) {
        json::Value::Object o;
        o["id"] = json::Value::String(p.id);
        o["kind"] = json::Value::String(p.kind);
        o["name"] = json::Value::String(p.name);
        o["description"] = json::Value::String(p.description);
        o["version"] = json::Value::Number(static_cast<double>(p.version));
        o["createdAtMs"] = json::Value::Number(static_cast<double>(p.createdAtMs));
        o["settings"] = p.settings;
        arr.push_back(json::Value(std::move(o)));
    }
    return DatabaseManager::Instance().Put("profiles", "items", json::Value(std::move(arr)));
}

Result<void> ProfileManager::Load() {
    auto doc = DatabaseManager::Instance().Get("profiles", "items");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.clear();
    if (doc.value().type() != json::Value::Type::Array) return Ok();
    for (const auto& e : *doc.value().asArray()) {
        Profile p;
        if (const auto* n = e.Find("id")) p.id = std::string(n->asString());
        if (const auto* n = e.Find("kind")) p.kind = std::string(n->asString());
        if (const auto* n = e.Find("name")) p.name = std::string(n->asString());
        if (const auto* n = e.Find("description")) p.description = std::string(n->asString());
        if (const auto* n = e.Find("version")) p.version = static_cast<int>(n->asInt(1));
        if (const auto* n = e.Find("createdAtMs")) p.createdAtMs = n->asInt(0);
        if (const auto* n = e.Find("settings")) p.settings = *n;
        profiles_.push_back(p);
    }
    return Ok();
}

size_t ProfileManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_.size();
}

} // namespace bps::project
