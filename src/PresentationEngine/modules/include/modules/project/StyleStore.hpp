#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <vector>

namespace bps::project {
struct StoredShowCategoryTemplate {
    std::string category;
    std::string templateKey;
    bool overrideEnabled = false;
};

struct StoredStyle {
    std::string id;                       // stable id ("s<n>", never reused)
    std::string name;
    std::string res;                      // display string ("1920×1080")
    std::string contentType = "shows";    // the style's own type (which picker lists it in)
    std::string templateKey;              // "" = no template picked; the engine's fallback layout applies
    std::string backgroundColor = "transparent";
    std::string backgroundImage;          // absolute file path; "" = none
    bool clearBackgroundOnText = false;
    bool showTemplates[5] = {false, false, false, false, false};
    std::string templateKeys[4];                        // shows | media | scripture | table
    // Shows-only category label (free text; FreeShow styles name a category).
    std::string category;
    // Optional per-show-library-category template overrides. A row is retained
    // while its override is off so the user can prepare the template first.
    std::vector<StoredShowCategoryTemplate> showCategoryTemplates;
    bool onBoard = false;
};

class StyleStore {
public:
    static StyleStore& Instance();

    // The whole roster, display order (empty when nothing was saved yet).
    std::vector<StoredStyle> Get() const;

    Result<void> Save(const std::vector<StoredStyle>& styles) const;

    static std::string ReattachBackgroundImage(const std::string& stored);

private:
    StyleStore() = default;

    static json::Value StyleToJson(const StoredStyle& s);
    static Result<StoredStyle> StyleFromJson(const json::Value& v);
};
} // namespace bps::project