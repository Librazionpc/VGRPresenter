#include "modules/presentation/PresentationValidator.hpp"

#include <algorithm>
#include <set>

namespace bps::presentation {

bool PresentationValidator::HasErrors(const std::vector<ValidationIssue>& issues) {
    for (const auto& i : issues)
        if (i.severity >= 1) return true;
    return false;
}

std::vector<ValidationIssue> PresentationValidator::Validate(
    const Presentation& presentation, const std::vector<std::string>* knownAssetIds) const {
    std::vector<ValidationIssue> issues;

    if (presentation.slides.empty()) {
        issues.push_back(ValidationIssue{{}, "presentation has no slides", 1});
        return issues;
    }

    std::set<std::string> seen;
    for (const auto& slide : presentation.slides) {
        if (slide.id.empty()) {
            issues.push_back(ValidationIssue{slide.id, "slide has an empty id", 1});
            continue;
        }
        if (!seen.insert(slide.id).second)
            issues.push_back(ValidationIssue{slide.id, "duplicate slide id", 1});

        // Media/font references: hard error when the asset set is known and the
        // reference is missing; warning when the set is unknown (can't verify).
        for (const auto& asset : slide.assetIds) {
            if (knownAssetIds) {
                if (std::find(knownAssetIds->begin(), knownAssetIds->end(), asset) ==
                    knownAssetIds->end())
                    issues.push_back(ValidationIssue{slide.id,
                                                     "missing asset reference '" + asset + "'", 1});
            } else {
                issues.push_back(ValidationIssue{slide.id,
                                                 "unresolved asset reference '" + asset + "'", 0});
            }
        }
        if (slide.title.empty() && slide.text.empty())
            issues.push_back(
                ValidationIssue{slide.id, "slide has no title and no content", 0});
    }
    return issues;
}

} // namespace bps::presentation
