#pragma once

// Scene Composition Engine (Phase 11, docs/specs/23). The definitive
// architecture: everything is a Scene. Scene → Regions → Layers → Widgets →
// Components. Layouts tell regions where to appear per output. Themes own all
// styling. Rules decide what appears where. Content never owns style.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <map>
#include <string>
#include <vector>

namespace bps::scene {

// --- Region (a named area of a scene) ------------------------------------------
struct Region {
    std::string id;
    std::string name;
    int order = 0;                     // z-order within the scene
    bool enabled = true;
};

// --- Anchors (docs/specs/23 §Constraints) ---------------------------------------
enum class Anchor : int {
    TopLeft = 0,
    TopCenter,
    TopRight,
    Center,
    BottomLeft,
    BottomCenter,
    BottomRight,
    Fill,
};

inline const char* ToString(Anchor a) {
    switch (a) {
        case Anchor::TopLeft:     return "TopLeft";
        case Anchor::TopCenter:   return "TopCenter";
        case Anchor::TopRight:    return "TopRight";
        case Anchor::Center:      return "Center";
        case Anchor::BottomLeft:  return "BottomLeft";
        case Anchor::BottomCenter:return "BottomCenter";
        case Anchor::BottomRight: return "BottomRight";
        case Anchor::Fill:        return "Fill";
    }
    return "Unknown";
}

// --- Layout: where regions appear on an output ----------------------------------
struct LayoutItem {
    std::string regionId;
    Anchor anchor = Anchor::Center;
    float xPct = 0.0f;                // percentage-based positioning
    float yPct = 0.0f;
    float widthPct = 0.0f;            // 0 = auto
    float heightPct = 0.0f;
    int xPx = 0;                      // absolute pixel offsets
    int yPx = 0;
    int widthPx = 0;
    int heightPx = 0;
    float opacity = 1.0f;
};

struct SceneLayout {
    std::string id;
    std::string name;
    std::string outputId;             // which output this layout targets
    std::vector<LayoutItem> items;
    bool safeArea = true;             // clamp to title-safe area
};

// --- Theme (docs/specs/23 §Themes): styling, never in content -------------------
struct SceneTheme {
    std::string id;
    std::string name;
    rendering::Color background{0.06f, 0.07f, 0.09f, 1.0f};
    std::string fontFamily = "default";
    float baseFontSize = 48.0f;
    rendering::Color primaryText{1, 1, 1, 1};
    rendering::Color secondaryText{0.75f, 0.75f, 0.75f, 1.0f};
    rendering::Color accent{0.2f, 0.6f, 1.0f, 1.0f};
    float padding = 40.0f;
    float borderRadius = 0.0f;
    std::map<std::string, std::string, std::less<>> extras;
};

// --- Widget definition ------------------------------------------------------------
struct WidgetDef {
    std::string id;
    std::string type;                 // "clock", "lyrics", "logo", "image"...
    std::string regionId;
    std::string contentId;            // linked content (song id, asset id...)
    std::vector<std::string> visibilityOutputs;   // which outputs see this widget
    bool visible = true;
    std::string styleKey;             // theme look-up key
};

// --- Template (a scene blueprint, docs/specs/23 §Templates) ----------------------
struct SceneTemplate {
    std::string id;
    std::string name;
    std::string contentType;          // "song", "bible", "announcement"...
    std::string baseTemplateId;       // inheritance
    std::vector<Region> regions;
    std::vector<WidgetDef> widgets;
    std::vector<SceneLayout> layouts; // per output
    std::string themeId;
};

// --- Rule (docs/specs/23 §Rule engine) --------------------------------------------
struct SceneRule {
    std::string id;
    std::string contentType;          // "" = any
    std::string outputId;             // "" = any
    std::string templateId;           // select this template
    std::string layoutId;             // select this layout
    std::string themeId;              // select this theme ("" = keep)
    std::string description;
};

// --- Composed scene (result of resolution) -----------------------------------------
struct ComposedScene {
    std::string sceneId;
    std::string contentId;
    std::string contentType;
    std::string templateId;
    std::string themeId;
    std::vector<WidgetDef> widgets;   // resolved + visible
    std::vector<SceneLayout> layouts; // resolved per output
    std::string appliedRuleId;
};

} // namespace bps::scene
