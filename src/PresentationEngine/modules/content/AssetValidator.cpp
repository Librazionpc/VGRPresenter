#include "modules/content/AssetValidator.hpp"

#include <set>

namespace bps::content {

bool AssetValidator::ValidName(std::string_view name) {
    return !name.empty() && name.size() <= 256;
}

bool AssetValidator::ValidUuid(const Uuid& u) {
    return u != Uuid{} && u.ToString().size() == 36;
}

ValidationReport AssetValidator::Validate(
    const AssetMetadata& meta,
    const std::function<std::optional<bool>(const std::string&)>& exists,
    const std::function<std::optional<uint64_t>(const std::string&)>& sizeOf) const {
    ValidationReport report;
    if (!ValidUuid(meta.uuid))
        report.issues.push_back({"invalid_metadata", "uuid missing or malformed"});
    if (!ValidName(meta.name))
        report.issues.push_back({"invalid_metadata", "name empty or too long"});
    if (meta.path.empty())
        report.issues.push_back({"invalid_metadata", "path missing"});
    if (exists) {
        auto e = exists(meta.path);
        if (e && !*e)
            report.issues.push_back({"missing_file", "file does not exist: " + meta.path});
        else if (sizeOf) {
            auto s = sizeOf(meta.path);
            if (s && *s == 0)
                report.issues.push_back({"empty_file", "file is empty: " + meta.path});
        }
    }
    if (!meta.checksum.empty() && meta.checksum.size() != 8)
        report.issues.push_back({"checksum_mismatch", "invalid checksum length"});
    if (!meta.hash.empty() && meta.hash.size() != 16)
        report.issues.push_back({"checksum_mismatch", "invalid hash length"});
    return report;
}

ValidationReport AssetValidator::ValidateLibrary(
    const std::vector<AssetMetadata>& library) const {
    ValidationReport report;
    std::set<std::string> seen;
    for (const auto& m : library) {
        std::string id = m.uuid.ToString();
        if (seen.count(id))
            report.issues.push_back({"duplicate_uuid", "duplicate uuid " + id});
        seen.insert(id);
        if (!ValidName(m.name))
            report.issues.push_back({"invalid_metadata", "invalid name on " + id});
    }
    return report;
}

} // namespace bps::content
