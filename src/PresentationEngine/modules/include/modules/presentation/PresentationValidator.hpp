#pragma once

// PresentationValidator (docs/specs/19 §Validator). Detects missing media/fonts,
// broken references, duplicate IDs, invalid layouts — reporting warnings and
// errors without crashing the engine. Content-type agnostic: it validates a
// generic Presentation document.

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <string>
#include <vector>

namespace bps::presentation {

class PresentationValidator {
public:
    // Validates a presentation. `knownAssetIds` (when provided) lets the
    // validator flag referenced-but-missing media/fonts; when null, asset
    // references are reported as warnings only (unresolved).
    // Returns issues (severity 0 = warning, 1 = error).
    std::vector<ValidationIssue> Validate(
        const Presentation& presentation,
        const std::vector<std::string>* knownAssetIds = nullptr) const;

    // True when the issue list contains any error-severity issue.
    static bool HasErrors(const std::vector<ValidationIssue>& issues);
};

} // namespace bps::presentation
