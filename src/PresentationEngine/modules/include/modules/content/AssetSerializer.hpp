#pragma once

// AssetSerializer (docs/specs/13 §Serialization): metadata JSON round-trip with
// a schema-version field + migration hook. Uses the Core json (03).

#include "modules/content/AssetMetadata.hpp"
#include "core/config/Json.hpp"

namespace bps::content {

constexpr int kMetadataSchemaVersion = 1;

class AssetSerializer {
public:
    // Serialize a single metadata record to a JSON value.
    static json::Value ToJson(const AssetMetadata& meta);

    // Deserialize; fails with Content_ValidationFailed on invalid records.
    static Result<AssetMetadata> FromJson(const json::Value& v);

    // Serialize a whole library (version header + array of records).
    static json::Value LibraryToJson(const std::vector<AssetMetadata>& library);
    static Result<std::vector<AssetMetadata>> LibraryFromJson(const json::Value& v);

    // Migration hook: returns a copy of `doc` upgraded to targetVersion.
    // Currently schema v1 only; future versions add migration steps here
    // without changing callers.
    static Result<json::Value> Migrate(json::Value doc, int targetVersion);
};

} // namespace bps::content
