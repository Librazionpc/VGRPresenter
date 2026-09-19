#include "modules/content/AssetSerializer.hpp"

#include <cstdint>
#include <string>
#include <format>

namespace bps::content {

namespace {

json::Value StrVec(const std::vector<std::string>& v) {
    json::Value::Array arr;
    arr.reserve(v.size());
    for (const auto& s : v) arr.push_back(json::Value::String(s));
    return json::Value(std::move(arr));
}

std::vector<std::string> ReadStrVec(const json::Value& v) {
    std::vector<std::string> out;
    const auto* arr = v.asArray();
    if (!arr) return out;
    for (const auto& e : *arr)
        out.push_back(std::string(e.asString()));
    return out;
}

} // namespace

json::Value AssetSerializer::ToJson(const AssetMetadata& meta) {
    json::Value::Object o;
    o["uuid"] = json::Value::String(meta.uuid.ToString());
    o["name"] = json::Value::String(meta.name);
    o["type"] = json::Value::String(ToString(meta.type));
    o["path"] = json::Value::String(meta.path);
    o["vfs"] = json::Value::String(meta.vfs);
    o["sizeBytes"] = json::Value::Number(static_cast<double>(meta.sizeBytes));
    o["hash"] = json::Value::String(meta.hash);
    o["checksum"] = json::Value::String(meta.checksum);
    o["createdMs"] = json::Value::Number(static_cast<double>(meta.createdMs));
    o["modifiedMs"] = json::Value::Number(static_cast<double>(meta.modifiedMs));
    o["tags"] = StrVec(meta.tags);
    o["category"] = json::Value::String(meta.category);
    o["author"] = json::Value::String(meta.author);
    o["description"] = json::Value::String(meta.description);
    o["version"] = json::Value::Number(static_cast<double>(meta.version));
    o["favorite"] = json::Value::Bool(meta.favorite);
    o["collections"] = StrVec(meta.collections);
    {
        json::Value::Array deps;
        for (const auto& d : meta.dependencies)
            deps.push_back(json::Value::String(d.ToString()));
        o["dependencies"] = json::Value(std::move(deps));
    }
    o["state"] = json::Value::String(ToString(meta.state));
    return json::Value(std::move(o));
}

Result<AssetMetadata> AssetSerializer::FromJson(const json::Value& v) {
    if (!v.Has("uuid") || !v.Has("name"))
        return Error::Make(Err::Content_ValidationFailed, "CAMS", "asset record missing uuid/name");
    AssetMetadata m;
    m.uuid = Uuid::FromString(std::string(v.Find("uuid")->asString()));
    m.name = std::string(v.Find("name")->asString());
    if (v.Has("type")) m.type = AssetTypeFromString(v.Find("type")->asString());
    if (v.Has("path")) m.path = std::string(v.Find("path")->asString());
    if (v.Has("vfs")) m.vfs = std::string(v.Find("vfs")->asString());
    if (v.Has("sizeBytes")) m.sizeBytes = static_cast<uint64_t>(v.Find("sizeBytes")->asInt());
    if (v.Has("hash")) m.hash = std::string(v.Find("hash")->asString());
    if (v.Has("checksum")) m.checksum = std::string(v.Find("checksum")->asString());
    if (v.Has("createdMs")) m.createdMs = v.Find("createdMs")->asInt();
    if (v.Has("modifiedMs")) m.modifiedMs = v.Find("modifiedMs")->asInt();
    if (v.Has("tags")) m.tags = ReadStrVec(*v.Find("tags"));
    if (v.Has("category")) m.category = std::string(v.Find("category")->asString());
    if (v.Has("author")) m.author = std::string(v.Find("author")->asString());
    if (v.Has("description")) m.description = std::string(v.Find("description")->asString());
    if (v.Has("version")) m.version = static_cast<int>(v.Find("version")->asInt());
    if (v.Has("favorite")) m.favorite = v.Find("favorite")->asBool();
    if (v.Has("collections")) m.collections = ReadStrVec(*v.Find("collections"));
    if (v.Has("dependencies")) {
        const auto* arr = v.Find("dependencies")->asArray();
        if (arr) {
            for (const auto& e : *arr)
                m.dependencies.push_back(Uuid::FromString(std::string(e.asString())));
        }
    }
    // state is runtime-only (not persisted); stays at its default.
    return Result<AssetMetadata>{m};
}

json::Value AssetSerializer::LibraryToJson(const std::vector<AssetMetadata>& library) {
    json::Value::Object o;
    o["schemaVersion"] = json::Value::Number(kMetadataSchemaVersion);
    json::Value::Array items;
    items.reserve(library.size());
    for (const auto& m : library) items.push_back(ToJson(m));
    o["assets"] = json::Value(std::move(items));
    return json::Value(std::move(o));
}

Result<std::vector<AssetMetadata>> AssetSerializer::LibraryFromJson(const json::Value& v) {
    auto migrated = Migrate(v, kMetadataSchemaVersion);
    if (!migrated.ok()) return migrated.error();
    const auto* arr = migrated.value().Find("assets");
    if (!arr || !arr->asArray())
        return Error::Make(Err::Content_ValidationFailed, "CAMS", "library missing assets array");
    std::vector<AssetMetadata> out;
    for (const auto& e : *arr->asArray()) {
        auto m = FromJson(e);
        if (!m.ok()) return m.error();
        out.push_back(m.value());
    }
    return Result<std::vector<AssetMetadata>>{std::move(out)};
}

Result<json::Value> AssetSerializer::Migrate(json::Value doc, int targetVersion) {
    int current = doc.Has("schemaVersion") ? static_cast<int>(doc.Find("schemaVersion")->asInt()) : 1;
    if (current > targetVersion)
        return Error::Make(Err::VersionMismatch, "CAMS",
                           std::format("library schema v{} is newer than supported v{}",
                                        current, targetVersion));
    // Future migrations: step `current` → current+1 → ... → targetVersion here.
    // v1 is the baseline; nothing to do.
    return Result<json::Value>{std::move(doc)};
}

} // namespace bps::content
