#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>

namespace bps {

// Configuration scope stack (docs/specs/03). Higher overrides lower.
enum class ConfigScope : int {
    Global = 0,   // machine-wide
    User,         // per OS user
    Workspace,    // per workspace folder
    Project,      // per presentation project
    Temporary,    // session-scoped
    Runtime       // in-memory overrides (highest)
};

inline const char* ToString(ConfigScope s) {
    switch (s) {
        case ConfigScope::Global:    return "Global";
        case ConfigScope::User:      return "User";
        case ConfigScope::Workspace: return "Workspace";
        case ConfigScope::Project:   return "Project";
        case ConfigScope::Temporary: return "Temporary";
        case ConfigScope::Runtime:   return "Runtime";
    }
    return "Unknown";
}

class ConfigurationManager final : public IService {
public:
    using IService::Reload;
    using IService::Reset;

    static ConfigurationManager& Instance();

    // IService contract: initialize with empty defaults and no global file.
    Result<void> Initialize() override;

    // defaults: schema/defaults tree seeded into every scope's lookup.
    // globalPath: optional base config file loaded into the Global scope.
    Result<void> Initialize(const json::Value& defaults, std::string_view globalPath = {});

    Result<void> SetScopePath(ConfigScope scope, std::string path);
    Result<void> Load(ConfigScope scope);       // parse + apply from its path
    Result<void> Save(ConfigScope scope);       // serialize scope to its path
    Result<void> Reload(ConfigScope scope);     // hot reload (00 §5)
    Result<void> Reset(ConfigScope scope);      // clear to defaults

    // --- Migration (03 §Migrate) ---
    // A migration transforms the scope document in place; the schema version is
    // stamped into the document under the reserved `_schema` key. Migrate runs
    // every registered migration whose target version is above the current one,
    // in ascending order.
    using MigrationFn = std::function<Result<void>(json::Value& doc)>;
    Result<void> AddMigration(int toVersion, MigrationFn fn);
    Result<void> Migrate(ConfigScope scope);

    struct MigrationEntry {
        int toVersion;
        MigrationFn fn;
    };

    // Typed, resolved access across the scope stack.
    bool Has(std::string_view key) const;
    json::Value Get(std::string_view key) const;
    bool GetBool(std::string_view key, bool dflt = false) const;
    long long GetInt(std::string_view key, long long dflt = 0) const;
    double GetDouble(std::string_view key, double dflt = 0.0) const;
    std::string GetString(std::string_view key, std::string dflt = {}) const;

    Result<void> Set(std::string_view key, json::Value value,
                     ConfigScope scope = ConfigScope::Runtime);

    // Profiles: ApplyProfile(name) loads <profileDir>/<name>.json over Global.
    Result<void> SetProfileDir(std::string dir);
    Result<void> ApplyProfile(std::string_view name);

    std::string ActiveProfile() const { return profile_; }

    Result<void> Validate() const;             // structural validation of scopes
    Result<void> PollWatch();                  // mtime-based hot reload (call from scheduler)

    const char* ServiceName() const noexcept override { return "ConfigurationManager"; }
    HealthReport GetHealth() const override;

private:
    ConfigurationManager() = default;
    Result<void> LoadFileInto(ConfigScope scope, const std::string& path);

    mutable std::mutex mutex_;
    std::map<ConfigScope, json::Value> scopes_;                 // guarded by mutex_
    std::map<ConfigScope, std::string> scopePaths_;             // guarded by mutex_
    std::map<ConfigScope, std::filesystem::file_time_type> mtimes_; // guarded by mutex_
    json::Value defaults_;                                      // guarded by mutex_
    std::vector<MigrationEntry> migrations_;                    // guarded by mutex_
    std::string profileDir_;
    std::string profile_;
    std::atomic<uint64_t> loadErrors_{0};
};

} // namespace bps
