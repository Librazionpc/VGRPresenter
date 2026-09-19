#pragma once

// PresentationNavigator (docs/specs/19 §Navigation). Next / Previous / First /
// Last / Jump by index / by stable slide id / by tag / by section / search.
// Keeps a navigation history for "Back" and recovery. Slides use stable IDs —
// never array indexes.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::presentation {

class PresentationNavigator {
public:
    // Binds the navigator to a presentation (visible slide order).
    void Open(const Presentation& presentation);

    size_t Current() const;
    size_t Count() const;                 // visible slides
    const Slide* CurrentSlide() const;
    const Presentation* BoundPresentation() const { return pres_; }

    Result<void> Next();
    Result<void> Previous();
    Result<void> First();
    Result<void> Last();
    Result<void> JumpTo(size_t index);              // visible index
    Result<void> JumpById(std::string_view slideId);
    Result<void> JumpByTag(std::string_view tag);
    Result<void> JumpBySection(std::string_view section);

    // First slide whose title/text/tags contain `query` (case-insensitive).
    Result<size_t> Search(std::string_view query) const;

    // Navigation history (most recent last). Supports undo-like "Back".
    Result<void> Back();
    size_t HistoryDepth() const;
    void ClearHistory();

private:
    const Presentation* pres_ = nullptr;
    std::vector<size_t> order_;   // visible slide indexes, in presentation order
    size_t current_ = 0;
    std::vector<size_t> history_; // previous positions
};

} // namespace bps::presentation
