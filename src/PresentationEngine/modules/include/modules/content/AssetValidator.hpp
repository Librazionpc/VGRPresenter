#pragma once

#include "modules/content/AssetMetadata.hpp"
#include "core/common/Common.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bps::content {
struct ValidationIssue {
    std::string code;       // "missing_file" / "empty_file" / "unsupported_format" /
                            // "invalid_metadata" / "duplicate_uuid" / "duplicate_hash" /
                            // "version_mismatch" / "checksum_mismatch"
    std::string message;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    bool Valid() const { return issues.empty(); }
};

class AssetValidator {
public:
    ValidationReport Validate(
        const AssetMetadata& meta,
        const std::function<std::optional<bool>(const std::string&)>& exists,
        const std::function<std::optional<uint64_t>(const std::string&)>& sizeOf) const;

    // Validate a whole library for duplicate UUIDs + invalid metadata.
    ValidationReport ValidateLibrary(const std::vector<AssetMetadata>& library) const;

private:
    static bool ValidName(std::string_view name);
    static bool ValidUuid(const Uuid& u);
};
} // namespace bps::content