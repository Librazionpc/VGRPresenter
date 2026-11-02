#pragma once

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <vector>

namespace bps::presentation {
class PresentationValidator {
public:
    std::vector<ValidationIssue> Validate(
        const Presentation& presentation,
        const std::vector<std::string>* knownAssetIds = nullptr) const;

    // True when the issue list contains any error-severity issue.
    static bool HasErrors(const std::vector<ValidationIssue>& issues);
};
} // namespace bps::presentation