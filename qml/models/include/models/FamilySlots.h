#pragma once

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