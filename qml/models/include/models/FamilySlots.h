#pragma once

// THE FAMILY → SLOT TABLE ON THE QML SIDE, and the ONE list of what a style calls
// each content family's gate and template pick.
//
// The engine owns WHICH SLOT a family is (presentation::kFamilySlot* and
// SlideRules::FamilyTemplateSlot — the one place a page's content tag becomes a
// slot); this table delegates to it and adds the only other thing the QML layer
// needs: the property NAMES. Both used to be spelled out in several places — the
// style model's load, save, two setters and its variant map, the output model's
// "familyTemplate" + capitalised-tag concatenation, and the style spec builder's
// per-family reads — so a fifth family, or a renamed property, needed every one
// of them edited in step, and a missed one styled the WRONG family in silence
// (the output model BUILT its key by capitalising the content type, so a tag that
// did not follow that convention named no property at all and the style quietly
// fell back to "the slides' own template governs").
//
// Rows are in SLOT order, which the static_assert below pins to the engine's — a
// loop over this table walks the families exactly as the engine's arrays are
// indexed.

#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SlideRules.hpp"

#include <QString>

#include <array>
#include <cstddef>

namespace FamilySlots {

struct Family {
    const char *tag;               // the slide's / the tab's content type
    const char *showProperty;      // the style's own gate for it
    const char *templateProperty;  // the style's template pick for it
};

inline constexpr int kCount = static_cast<int>(bps::presentation::kFamilySlotCount);

inline const std::array<Family, static_cast<std::size_t>(kCount)> &Rows()
{
    static const std::array<Family, static_cast<std::size_t>(kCount)> rows{ {
        { "shows",     "showShows",     "familyTemplateShows" },
        { "media",     "showMedia",     "familyTemplateMedia" },
        { "scripture", "showScripture", "familyTemplateScripture" },
        { "table",     "showTable",     "familyTemplateTable" },
    } };
    return rows;
}

// The row order IS the engine's slot order: this keeps the two from drifting.
static_assert(bps::presentation::kFamilySlotShows == 0 && bps::presentation::kFamilySlotMedia == 1
                  && bps::presentation::kFamilySlotScripture == 2 && bps::presentation::kFamilySlotTable == 3
                  && kCount == 4,
              "FamilySlots' rows must be in the engine's family slot order");

// The slot a content type names, or -1 when it names no family: an unknown tag,
// or the Overlays pane's own tag (overlays are not a slide family — the style's
// showOverlay gate is a different axis and is read by name). The MATCHED tag's
// slot comes from the engine, so this table never decides it a second time.
inline int SlotFor(const QString &contentType)
{
    if (contentType.isEmpty())
        return -1;
    const auto &rows = Rows();
    for (int slot = 0; slot < kCount; ++slot) {
        const Family &row = rows[static_cast<std::size_t>(slot)];
        if (contentType == QLatin1String(row.tag))
            return static_cast<int>(bps::presentation::SlideRules::FamilyTemplateSlot(row.tag));
    }
    return -1;
}

// The row for a content type, or nullptr when it names no family.
inline const Family *RowFor(const QString &contentType)
{
    const int slot = SlotFor(contentType);
    return slot < 0 ? nullptr : &Rows()[static_cast<std::size_t>(slot)];
}

} // namespace FamilySlots
