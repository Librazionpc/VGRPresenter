#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {
struct StoredGroup {
    std::string id;                        // stable id ("g<n>", never reused)
    std::string name;
    std::string color = "#5825f5";
    std::string shortcut;                  // single letter, uppercase by convention; "" = none
    std::string templateId;
    bool isDefault = false;
};

class GroupStore {
public:
    static GroupStore& Instance();

    // The whole roster, display order (empty when nothing was saved yet).
    std::vector<StoredGroup> Get() const;

    // Replaces the roster and persists it. Returns the persistence result so a
    // failed write surfaces (the in-memory store is still updated).
    Result<void> Save(const std::vector<StoredGroup>& groups) const;

private:
    GroupStore() = default;
};
} // namespace bps::project