#pragma once

// ProfileManager (docs/specs/15 §Profile Manager): stores reusable user
// preference profiles — display profiles, import profiles, notification
// profiles, rendering profiles. Each profile is a named, versioned JSON
// document; applying one publishes the relevant ConfigurationManager keys.

#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct Profile {
    std::string id;
    std::string kind;      // "display" | "import" | "notification" | "rendering"
    std::string name;
    std::string description;
    int version = 1;
    int64_t createdAtMs = 0;
    json::Value settings;  // key → value (flat dot-keys, like the ConfigurationManager)
};

class ProfileManager {
public:
    static ProfileManager& Instance();

    Result<std::string> Create(std::string_view kind, std::string_view name,
                               const json::Value& settings, std::string_view description = {});
    std::vector<Profile> List(std::string_view kind = {}) const;
    Result<Profile> Get(std::string_view profileId) const;
    Result<void> Remove(std::string_view profileId);
    Result<void> UpdateSettings(std::string_view profileId, const json::Value& settings);

    // Apply a profile: writes its settings into the ConfigurationManager and
    // marks it as the active profile for its kind.
    Result<void> Apply(std::string_view profileId);
    Result<Profile> Active(std::string_view kind) const;

    Result<void> Save();
    Result<void> Load();
    size_t Count() const;

private:
    ProfileManager() = default;

    mutable std::mutex mutex_;
    std::vector<Profile> profiles_;
};

} // namespace bps::project
