#pragma once

// ActionStore (Settings · Appearance & Actions · "Manage actions" persistence):
// the kernel-side copy of the user's own named Actions (FreeShow's Functions >
// Actions, scoped for this app to CLICK + KEYPRESS activation only — MIDI/OSC/
// REST/OBS triggers are a later phase). Each Action is one real app capability
// (ActionListModel's own catalog, mirroring LiveOutputService's Q_INVOKABLEs)
// bound to an optional single-letter live shortcut.
//
// Storage is a JSON ARRAY OF OBJECTS (stable ids, order = display order):
//   [ { "id": "a1", "name": "Clear everything", "icon": "clear",
//       "effectId": "clear_all", "effectValue": "", "shortcut": "Escape" } ]

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {

struct StoredAction {
    std::string id;                        // stable id ("a<n>", never reused)
    std::string name;
    std::string icon = "flag";
    // One of ActionListModel's catalog ids ("clear_all", "jump_to_group"...).
    // ONE effect per action for now — FreeShow chains several per action;
    // this app's version runs exactly one, the common case, until chaining
    // earns its own complexity.
    std::string effectId;
    // The effect's own parameter, when it takes one (jump_to_group's group
    // name; most effects take none and leave this empty).
    std::string effectValue;
    std::string shortcut;                  // single key/chord; "" = none
    // A CUSTOM ACTIVATION (FreeShow's trigger): the event that runs this
    // action by itself, e.g. "show_opened". One of ActionListModel's own
    // trigger ids; "" = only the keypress (or the Functions pane) runs it.
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
