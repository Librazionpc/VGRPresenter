#pragma once

// GroupStore (Settings · Look & Feel · Manage groups persistence): the kernel-side
// copy of the group roster — named song-section tags ("verse", "chorus"...) a
// slide can carry, each with its own colour and an optional single-letter live
// shortcut (press it during GO LIVE to jump to the next slide carrying that
// group). Lives in the kernel's DatabaseManager document store ("groups"/
// "roster" in <dataDir>/kernel.json), same persistence shape as StyleStore.
//
// Storage is a JSON ARRAY OF OBJECTS (stable ids, order = display order):
//   [ { "id": "g1", "name": "verse", "color": "#5825f5", "shortcut": "V",
//       "isDefault": true } ]
// A missing/corrupt document reads as an empty roster; GroupListModel seeds the
// eight built-ins the first time it finds nothing saved (see its own comment).

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
    // The eight ship-with-the-app groups (FreeShow's own default set) — a
    // renamed default stops being one (see GroupListModel::renameGroup), the
    // same "editing a default detaches it" rule StyleItem's showShows etc.
    // don't need, but ManageGroups.svelte's own `delete a[id].default` does.
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
