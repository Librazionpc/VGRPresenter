#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {
struct StoredAction {
    std::string id;                        // stable id ("a<n>", never reused)
    std::string name;
    std::string icon = "flag";
    std::string effectId;
    // The effect's own parameter, when it takes one (jump_to_group's group
    // name; most effects take none and leave this empty).
    std::string effectValue;
    std::string shortcut;                  // single key/chord; "" = none
    std::string trigger;
    bool visibleInFunctions = true;
    struct Step {
        std::string effectId;
        std::string effectValue;
    };
    std::vector<Step> steps;                // ordered operations run by one action
};

class ActionStore {
public:
    static ActionStore& Instance();

    std::vector<StoredAction> Get() const;
    Result<void> Save(const std::vector<StoredAction>& actions) const;

private:
    ActionStore() = default;
};
} // namespace bps::project