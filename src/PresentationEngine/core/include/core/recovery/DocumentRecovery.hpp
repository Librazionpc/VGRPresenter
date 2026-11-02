#pragma once

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <string>
#include <string_view>

namespace bps::recovery {
// What happened to one damaged document.
struct DamageReport {
    std::string path;         // the document that was damaged
    std::string quarantine;   // where its bytes were set aside ("" = none)
    std::string reason;       // the failure that condemned it (parse error text)
    bool preserved = false;   // the original bytes are on disk under `quarantine`

    bool nothingToPreserve() const { return quarantine.empty(); }
};

std::string DocumentShapeProblem(const json::Value& root, std::string_view requiredKey,
                                 json::Value::Type requiredType);

DamageReport SetAsideDamagedDocument(std::string_view path, std::string_view module,
                                     std::string_view reason);
} // namespace bps::recovery