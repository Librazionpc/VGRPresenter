#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/display/DisplayTypes.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps::display {
class DisplayProfileStore {
public:
    // --- Lifecycle ---------------------------------------------------------------
    Result<void> Initialize();
    Result<void> Shutdown();

    // --- Registry -----------------------------------------------------------------
    Result<void> SaveProfile(const DisplayProfile& profile);   // upsert
    Result<void> DeleteProfile(std::string_view name);
    Result<DisplayProfile> GetProfile(std::string_view name) const;
    std::vector<std::string> ProfileNames() const;
    size_t Count() const;
    void Clear();

    // --- Serialization (JSON) ------------------------------------------------------
    static std::string ToJson(const DisplayProfile& profile);
    static Result<DisplayProfile> FromJson(std::string_view json);

private:
    mutable std::mutex mutex_;
    std::map<std::string, DisplayProfile, std::less<>> profiles_;
    bool initialized_ = false;
};
} // namespace bps::display