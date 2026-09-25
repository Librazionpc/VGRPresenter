#include "modules/project/StyleStore.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/logging/Logger.hpp"

namespace bps::project {

namespace {
// Collection + key inside the kernel's DatabaseManager document store
// (<dataDir>/kernel.json). Kept as free constants (not class members) so both
// halves of StyleToJson/StyleFromJson round-tripping can't drift.
constexpr const char* kCollection = "styles";
constexpr const char* kKey = "roster";
} // namespace

StyleStore& StyleStore::Instance() {
    static StyleStore instance;
    return instance;
}

std::vector<StoredStyle> StyleStore::Get() const {
    std::vector<StoredStyle> out;
    auto doc = DatabaseManager::Instance().Get(kCollection, kKey);
    if (!doc.ok())
        return out;   // nothing saved yet — an empty roster is not an error

    const json::Value::Array* arr = doc.value().asArray();
    if (!arr)
        return out;   // corrupt shape: start fresh rather than fail the boot

    out.reserve(arr->size());
    for (const json::Value& v : *arr) {
        if (auto s = StyleFromJson(v); s.ok())
            out.push_back(std::move(s.value()));
    }
    return out;
}

Result<void> StyleStore::Save(const std::vector<StoredStyle>& styles) {
    json::Value::Array arr;
    arr.reserve(styles.size());
    for (const StoredStyle& s : styles)
        arr.push_back(StyleToJson(s));
    auto r = DatabaseManager::Instance().Put(kCollection, kKey, json::Value(std::move(arr)));
    if (!r.ok())
        return r;
    // Put() only updates memory; Flush() writes the file. Saving without the
    // flush would lose the roster to a crash — and a crash is exactly when
    // the user notices styles "didn't save".
    return DatabaseManager::Instance().Flush();
}

json::Value StyleStore::StyleToJson(const StoredStyle& s) {
    json::Value::Object o;
    o["id"] = json::Value::String(s.id);
    o["name"] = json::Value::String(s.name);
    o["res"] = json::Value::String(s.res);
    o["contentType"] = json::Value::String(s.contentType);
    o["templateKey"] = json::Value::String(s.templateKey);
    o["backgroundColor"] = json::Value::String(s.backgroundColor);
    o["clearBackgroundOnText"] = json::Value::Bool(s.clearBackgroundOnText);
    return json::Value(std::move(o));
}

Result<StoredStyle> StyleStore::StyleFromJson(const json::Value& v) {
    const json::Value::Object* o = v.asObject();
    if (!o)
        return Error::Make(Err::InvalidArgument, "StyleStore", "style entry is not an object");

    StoredStyle s;
    s.id = std::string(v.Find("id")->asString());
    s.name = std::string(v.Find("name")->asString());
    s.res = std::string(v.Find("res")->asString());
    s.contentType = std::string(v.Find("contentType")->asString("shows"));
    s.templateKey = std::string(v.Find("templateKey")->asString("lowerThird"));
    s.backgroundColor = std::string(v.Find("backgroundColor")->asString("transparent"));
    s.clearBackgroundOnText = v.Find("clearBackgroundOnText")->asBool(false);
    // An entry without an id (hand-edited file, partial write) is skipped by
    // the caller — ids are the stable identity everything else keys on.
    if (s.id.empty())
        return Error::Make(Err::InvalidArgument, "StyleStore", "style entry has no id");
    return s;
}

} // namespace bps::project
