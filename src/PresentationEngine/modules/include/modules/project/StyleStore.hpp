#pragma once

// StyleStore (Settings · Styles persistence): the kernel-side copy of the
// styles roster. Lives in the kernel's DatabaseManager document store
// ("styles"/"roster" in <dataDir>/kernel.json — the same file the production
// graph and workspace state live in), so styles and the outputs assigned to
// them survive a restart exactly like every other persisted production state.
//
// Storage is a JSON ARRAY OF OBJECTS (stable ids, order = display order):
//   [ { "id": "s1", "name": "Primary", "res": "1920×1080",
//       "contentType": "shows", "templateKey": "lowerThird",
//       "backgroundColor": "transparent", "clearBackgroundOnText": false } ]
// The UI's StyleListModel hydrates from Get() at construction and calls Save()
// after every mutation. A missing/corrupt document simply reads as an empty
// roster (the app starts fresh — same contract as the production graph).

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {

struct StoredStyle {
    std::string id;                       // stable id ("s<n>", never reused)
    std::string name;
    std::string res;                      // display string ("1920×1080")
    std::string contentType = "shows";    // shows | media | scripture | table
    std::string templateKey = "lowerThird";
    std::string backgroundColor = "transparent";
    bool clearBackgroundOnText = false;
};

class StyleStore {
public:
    static StyleStore& Instance();

    // The whole roster, display order (empty when nothing was saved yet).
    std::vector<StoredStyle> Get() const;

    // Replaces the roster and persists it. Returns the persistence result so
    // a failed write surfaces (the in-memory store is still updated).
    Result<void> Save(const std::vector<StoredStyle>& styles);

private:
    StyleStore() = default;

    static json::Value StyleToJson(const StoredStyle& s);
    static Result<StoredStyle> StyleFromJson(const json::Value& v);
};

} // namespace bps::project
