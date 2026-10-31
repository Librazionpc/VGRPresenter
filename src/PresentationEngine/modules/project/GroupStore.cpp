#include "modules/project/GroupStore.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/logging/Logger.hpp"

namespace bps::project {

namespace {
constexpr const char* kCollection = "groups";
constexpr const char* kKey = "roster";

json::Value GroupToJson(const StoredGroup& g) {
    json::Value::Object o;
    o["id"] = json::Value::String(g.id);
    o["name"] = json::Value::String(g.name);
    o["color"] = json::Value::String(g.color);
    o["shortcut"] = json::Value::String(g.shortcut);
    o["template"] = json::Value::String(g.templateId);
    o["isDefault"] = json::Value::Bool(g.isDefault);
    return json::Value(std::move(o));
}

Result<StoredGroup> GroupFromJson(const json::Value& v) {
    const json::Value::Object* o = v.asObject();
    if (!o)
        return Error::Make(Err::InvalidArgument, "GroupStore", "group entry is not an object");

    const auto str = [&v](std::string_view key, std::string_view dflt = {}) {
        if (const json::Value* f = v.Find(key))
            return std::string(f->asString(dflt));
        return std::string(dflt);
    };
    const auto flag = [&v](std::string_view key, bool dflt) {
        if (const json::Value* f = v.Find(key))
            return f->asBool(dflt);
        return dflt;
    };

    StoredGroup g;
    g.id = str("id");
    g.name = str("name");
    g.color = str("color", "#5825f5");
    g.shortcut = str("shortcut");
    g.templateId = str("template");
    g.isDefault = flag("isDefault", false);
    return g;
}
} // namespace

GroupStore& GroupStore::Instance() {
    static GroupStore instance;
    return instance;
}

std::vector<StoredGroup> GroupStore::Get() const {
    std::vector<StoredGroup> out;
    auto doc = DatabaseManager::Instance().Get(kCollection, kKey);
    if (!doc.ok())
        return out;   // nothing saved yet — an empty roster is not an error (GroupListModel seeds the defaults)

    const json::Value::Array* arr = doc.value().asArray();
    if (!arr)
        return out;   // corrupt shape: start fresh rather than fail the boot

    out.reserve(arr->size());
    for (const json::Value& v : *arr) {
        if (auto g = GroupFromJson(v); g.ok())
            out.push_back(std::move(g.value()));
    }
    return out;
}

Result<void> GroupStore::Save(const std::vector<StoredGroup>& groups) const {
    json::Value::Array arr;
    arr.reserve(groups.size());
    for (const StoredGroup& g : groups)
        arr.push_back(GroupToJson(g));
    auto r = DatabaseManager::Instance().Put(kCollection, kKey, json::Value(std::move(arr)));
    if (!r.ok())
        return r;
    // Put() only updates memory; Flush() writes the file — without it a crash
    // right after saving loses the roster (same contract StyleStore::Save follows).
    return DatabaseManager::Instance().Flush();
}

} // namespace bps::project
