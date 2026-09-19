#include "core/config/ConfigurationManager.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <format>

namespace bps {

ConfigurationManager& ConfigurationManager::Instance() {
    static ConfigurationManager instance;
    return instance;
}

Result<void> ConfigurationManager::Initialize() {
    return Initialize(json::Value(json::Value::Object{}), {});
}

Result<void> ConfigurationManager::Initialize(const json::Value& defaults,
                                              std::string_view globalPath) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        defaults_ = defaults;
        for (int i = 0; i <= static_cast<int>(ConfigScope::Runtime); ++i) {
            scopes_[static_cast<ConfigScope>(i)] = json::Value(json::Value::Object{});
        }
        if (!globalPath.empty()) {
            scopePaths_[ConfigScope::Global] = std::string(globalPath);
            path = scopePaths_[ConfigScope::Global];
        }
    }
    if (!path.empty()) return Load(ConfigScope::Global);
    return Ok();
}

Result<void> ConfigurationManager::SetScopePath(ConfigScope scope, std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    scopePaths_[scope] = std::move(path);
    return Ok();
}

Result<void> ConfigurationManager::LoadFileInto(ConfigScope scope, const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open())
        return Error::Make(Err::IoError, "ConfigurationManager", "cannot open: " + path);

    std::stringstream ss;
    ss << in.rdbuf();
    auto parsed = json::Parse(ss.str());
    if (!parsed.ok()) {
        loadErrors_.fetch_add(1);
        return Error::Make(Err::Config_ParseFailed, "ConfigurationManager",
                           "failed to parse " + path, &parsed.error());
    }
    if (parsed.value().type() != json::Value::Type::Object)
        return Error::Make(Err::Config_ParseFailed, "ConfigurationManager",
                           "config root must be an object: " + path);

    std::lock_guard<std::mutex> lock(mutex_);
    scopes_[scope] = parsed.value();
    mtimes_[scope] = std::filesystem::last_write_time(path);
    return Ok();
}

Result<void> ConfigurationManager::Load(ConfigScope scope) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = scopePaths_.find(scope);
        if (it == scopePaths_.end() || it->second.empty()) {
            // No file configured for this scope: initialize empty and treat as ok.
            scopes_[scope] = json::Value(json::Value::Object{});
            return Ok();
        }
        path = it->second;
    }
    return LoadFileInto(scope, path);
}

Result<void> ConfigurationManager::Save(ConfigScope scope) {
    std::string path;
    json::Value value;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = scopePaths_.find(scope);
        if (it == scopePaths_.end() || it->second.empty())
            return Error::Make(Err::InvalidArgument, "ConfigurationManager",
                               "no path configured for scope " + std::string(ToString(scope)));
        path = it->second;
        value = scopes_[scope];
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open())
        return Error::Make(Err::IoError, "ConfigurationManager", "cannot write: " + path);
    out << value.ToString() << "\n";
    if (!out.good())
        return Error::Make(Err::IoError, "ConfigurationManager", "write failed: " + path);
    return Ok();
}

Result<void> ConfigurationManager::Reload(ConfigScope scope) { return Load(scope); }

Result<void> ConfigurationManager::Reset(ConfigScope scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    scopes_[scope] = json::Value(json::Value::Object{});
    return Ok();
}

bool ConfigurationManager::Has(std::string_view key) const {
    return Get(key).type() != json::Value::Type::Null;
}

json::Value ConfigurationManager::Get(std::string_view key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Resolve from highest priority scope down to Global, then defaults.
    for (int i = static_cast<int>(ConfigScope::Runtime); i >= static_cast<int>(ConfigScope::Global); --i) {
        auto it = scopes_.find(static_cast<ConfigScope>(i));
        if (it == scopes_.end()) continue;
        if (const json::Value* v = it->second.Find(key)) return *v;
    }
    if (const json::Value* v = defaults_.Find(key)) return *v;
    return json::Value::Null();
}

bool ConfigurationManager::GetBool(std::string_view key, bool dflt) const {
    return Get(key).asBool(dflt);
}
long long ConfigurationManager::GetInt(std::string_view key, long long dflt) const {
    return Get(key).asInt(dflt);
}
double ConfigurationManager::GetDouble(std::string_view key, double dflt) const {
    return Get(key).asNumber(dflt);
}
std::string ConfigurationManager::GetString(std::string_view key, std::string dflt) const {
    return std::string(Get(key).asString(dflt));
}

Result<void> ConfigurationManager::Set(std::string_view key, json::Value value, ConfigScope scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value root = scopes_[scope];
    // Build the dot path into the object.
    json::Value::Object* cur = nullptr;
    // Ensure root is an object.
    if (root.type() != json::Value::Type::Object) root = json::Value(json::Value::Object{});
    cur = const_cast<json::Value::Object*>(root.asObject());

    std::string k(key);
    size_t start = 0;
    while (true) {
        size_t dot = k.find('.', start);
        if (dot == std::string::npos) {
            (*cur)[k.substr(start)] = std::move(value);
            break;
        }
        std::string part = k.substr(start, dot - start);
        auto& child = (*cur)[part];
        if (child.type() != json::Value::Type::Object)
            child = json::Value(json::Value::Object{});
        cur = const_cast<json::Value::Object*>(child.asObject());
        start = dot + 1;
    }
    scopes_[scope] = root;
    return Ok();
}

Result<void> ConfigurationManager::AddMigration(int toVersion, MigrationFn fn) {
    if (!fn) return Error::Make(Err::InvalidArgument, "ConfigurationManager", "null migration");
    std::lock_guard<std::mutex> lock(mutex_);
    migrations_.push_back(MigrationEntry{toVersion, std::move(fn)});
    std::stable_sort(migrations_.begin(), migrations_.end(),
                     [](const MigrationEntry& a, const MigrationEntry& b) {
                         return a.toVersion < b.toVersion;
                     });
    return Ok();
}

Result<void> ConfigurationManager::Migrate(ConfigScope scope) {
    json::Value root;
    std::vector<MigrationEntry> migrations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = scopes_[scope];
        if (root.type() != json::Value::Type::Object) root = json::Value(json::Value::Object{});
        migrations = migrations_;
    }
    int current =
        root.Find("_schema") ? static_cast<int>(root.Find("_schema")->asInt(0)) : 0;
    for (const auto& m : migrations) {
        if (m.toVersion <= current) continue;
        auto r = m.fn(root);
        if (!r.ok())
            return Error::Make(Err::Config_MigrationMissing, "ConfigurationManager",
                               std::format("migration to schema v{} failed for scope {}",
                                            m.toVersion, ToString(scope)),
                               &r.error());
        current = m.toVersion;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        json::Value::Object* obj = const_cast<json::Value::Object*>(root.asObject());
        if (obj) (*obj)["_schema"] = json::Value::Number(static_cast<double>(current));
        scopes_[scope] = root;
    }
    return Ok();
}

Result<void> ConfigurationManager::SetProfileDir(std::string dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    profileDir_ = std::move(dir);
    return Ok();
}

Result<void> ConfigurationManager::ApplyProfile(std::string_view name) {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = profileDir_;
        profile_ = std::string(name);
    }
    if (dir.empty())
        return Error::Make(Err::InvalidArgument, "ConfigurationManager", "profile dir not set");
    std::string path = dir + "/" + std::string(name) + ".json";
    return LoadFileInto(ConfigScope::Global, path);
}

Result<void> ConfigurationManager::Validate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [scope, value] : scopes_) {
        if (value.type() != json::Value::Type::Object)
            return Error::Make(Err::Config_ValidationFailed, "ConfigurationManager",
                               "scope " + std::string(ToString(scope)) + " is not an object");
    }
    return Ok();
}

Result<void> ConfigurationManager::PollWatch() {
    std::vector<std::pair<ConfigScope, std::string>> toReload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [scope, path] : scopePaths_) {
            if (path.empty()) continue;
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(path, ec);
            if (ec) continue;
            auto it = mtimes_.find(scope);
            if (it == mtimes_.end() || it->second != mtime) toReload.emplace_back(scope, path);
        }
    }
    for (auto& [scope, path] : toReload) {
        if (auto r = LoadFileInto(scope, path); !r.ok()) loadErrors_.fetch_add(1);
    }
    return Ok();
}

HealthReport ConfigurationManager::GetHealth() const {
    HealthReport r;
    r.errorCount = loadErrors_.load();
    r.state = loadErrors_.load() == 0 ? HealthState::Healthy : HealthState::Degraded;
    r.detail = loadErrors_.load() == 0 ? "config loaded" : "config load errors present";
    return r;
}

} // namespace bps
