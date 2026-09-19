#include "modules/project/Project.hpp"

#include <cstring>

namespace bps::project {

namespace {

int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

json::Value V(std::string_view s) { return json::Value::String(std::string(s)); }

} // namespace

json::Value ProjectToJson(const Project& p) {
    json::Value::Object o;
    o["id"] = V(p.id);
    o["name"] = V(p.name);
    o["path"] = V(p.path);
    o["description"] = V(p.description);
    o["schema"] = V(p.schema.ToString());
    o["archived"] = json::Value::Bool(p.archived);
    o["createdAtMs"] = json::Value::Number(static_cast<double>(p.createdAtMs));
    o["modifiedAtMs"] = json::Value::Number(static_cast<double>(p.modifiedAtMs));

    json::Value::Object st;
    st["theme"] = V(p.settings.theme);
    st["defaultTemplateId"] = V(p.settings.defaultTemplateId);
    st["aspectRatio"] = V(p.settings.aspectRatio);
    st["autosaveIntervalSec"] =
        json::Value::Number(static_cast<double>(p.settings.autosaveIntervalSec));
    st["compressAssets"] = json::Value::Bool(p.settings.compressAssets);
    st["generateThumbnails"] = json::Value::Bool(p.settings.generateThumbnails);
    if (!p.settings.extra.isNull()) st["extra"] = p.settings.extra;
    o["settings"] = json::Value(std::move(st));

    json::Value::Array docs;
    for (const auto& d : p.openDocuments) docs.push_back(V(d));
    o["openDocuments"] = json::Value(std::move(docs));

    json::Value::Array assets;
    for (const auto& a : p.assetUuids) assets.push_back(V(a));
    o["assetUuids"] = json::Value(std::move(assets));
    return json::Value(std::move(o));
}

Result<Project> ProjectFromJson(const json::Value& v) {
    Project p;
    auto get = [&](std::string_view key, std::string& out) {
        if (const auto* n = v.Find(key)) out = std::string(n->asString());
    };
    get("id", p.id);
    get("name", p.name);
    get("path", p.path);
    get("description", p.description);
    if (const auto* n = v.Find("schema")) p.schema = Version::Parse(n->asString());
    if (const auto* n = v.Find("archived")) p.archived = n->asBool(false);
    if (const auto* n = v.Find("createdAtMs")) p.createdAtMs = n->asInt(0);
    if (const auto* n = v.Find("modifiedAtMs")) p.modifiedAtMs = n->asInt(0);

    if (const auto* s = v.Find("settings")) {
        if (const auto* n = s->Find("theme")) p.settings.theme = std::string(n->asString());
        if (const auto* n = s->Find("defaultTemplateId"))
            p.settings.defaultTemplateId = std::string(n->asString());
        if (const auto* n = s->Find("aspectRatio")) p.settings.aspectRatio = std::string(n->asString());
        if (const auto* n = s->Find("autosaveIntervalSec"))
            p.settings.autosaveIntervalSec = static_cast<int>(n->asInt(60));
        if (const auto* n = s->Find("compressAssets")) p.settings.compressAssets = n->asBool(false);
        if (const auto* n = s->Find("generateThumbnails")) p.settings.generateThumbnails = n->asBool(true);
        if (const auto* n = s->Find("extra")) p.settings.extra = *n;
    }
    if (const auto* arr = v.Find("openDocuments")) {
        if (const auto* a = arr->asArray())
            for (const auto& e : *a) p.openDocuments.push_back(std::string(e.asString()));
    }
    if (const auto* arr = v.Find("assetUuids")) {
        if (const auto* a = arr->asArray())
            for (const auto& e : *a) p.assetUuids.push_back(std::string(e.asString()));
    }
    if (p.id.empty()) return Error::Make(Err::Project_NotFound, "Project", "missing id");
    if (p.createdAtMs == 0) p.createdAtMs = MsNow();
    return Result<Project>{p};
}

} // namespace bps::project
