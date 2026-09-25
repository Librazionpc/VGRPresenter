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
    o["backgroundImage"] = json::Value::String(s.backgroundImage);
    o["clearBackgroundOnText"] = json::Value::Bool(s.clearBackgroundOnText);
    json::Value::Object tmpl;
    tmpl["shows"] = json::Value::Bool(s.showTemplates[0]);
    tmpl["media"] = json::Value::Bool(s.showTemplates[1]);
    tmpl["scripture"] = json::Value::Bool(s.showTemplates[2]);
    tmpl["table"] = json::Value::Bool(s.showTemplates[3]);
    o["showTemplates"] = json::Value(std::move(tmpl));
    o["category"] = json::Value::String(s.category);
    return json::Value(std::move(o));
}

Result<StoredStyle> StyleStore::StyleFromJson(const json::Value& v) {
    const json::Value::Object* o = v.asObject();
    if (!o)
        return Error::Make(Err::InvalidArgument, "StyleStore", "style entry is not an object");

    // GUARDED READS: Find() returns nullptr for a missing key — every read of
    // a field an OLDER saved roster may not carry (backgroundImage,
    // showTemplates, category) must go through str()/flag(), or hydrating a
    // pre-existing roster null-derefs and takes the Styles screen down.
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

    StoredStyle s;
    s.id = str("id");
    s.name = str("name");
    s.res = str("res");
    s.contentType = str("contentType", "shows");
    s.templateKey = str("templateKey", "lowerThird");
    s.backgroundColor = str("backgroundColor", "transparent");
    s.backgroundImage = str("backgroundImage");
    s.clearBackgroundOnText = flag("clearBackgroundOnText", false);
    if (const json::Value* tmpl = v.Find("showTemplates"); tmpl && tmpl->asObject()) {
        const auto tflag = [tmpl](std::string_view key, bool dflt) {
            if (const json::Value* f = tmpl->Find(key))
                return f->asBool(dflt);
            return dflt;
        };
        s.showTemplates[0] = tflag("shows", true);
        s.showTemplates[1] = tflag("media", true);
        s.showTemplates[2] = tflag("scripture", true);
        s.showTemplates[3] = tflag("table", true);
    }
    s.category = str("category");
    // An entry without an id (hand-edited file, partial write) is skipped by
    // the caller — ids are the stable identity everything else keys on.
    if (s.id.empty())
        return Error::Make(Err::InvalidArgument, "StyleStore", "style entry has no id");
    return s;
}

} // namespace bps::project
