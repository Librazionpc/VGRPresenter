#include "modules/project/PresetStore.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/logging/Logger.hpp"

namespace bps::project {

namespace {
constexpr const char* kCollection = "presets";
constexpr const char* kKey = "roster";

json::Value PresetToJson(const StoredPreset& p) {
    json::Value::Object o;
    o["id"] = json::Value::String(p.id);
    o["name"] = json::Value::String(p.name);
    o["kind"] = json::Value::String(p.kind);
    o["config"] = json::Value::String(p.configJson);
    return json::Value(std::move(o));
}

Result<StoredPreset> PresetFromJson(const json::Value& v) {
    if (!v.asObject())
        return Error::Make(Err::InvalidArgument, "PresetStore", "preset entry is not an object");

    const auto str = [&v](std::string_view key, std::string_view dflt = {}) {
        if (const json::Value* f = v.Find(key))
            return std::string(f->asString(dflt));
        return std::string(dflt);
    };

    StoredPreset p;
    p.id = str("id");
    p.name = str("name");
    p.kind = str("kind", "recording");
    if (p.kind != "streaming" && p.kind != "audio")
        p.kind = "recording";
    p.configJson = str("config", "{}");
    if (p.id.empty())
        return Error::Make(Err::InvalidArgument, "PresetStore", "preset entry has no id");
    return p;
}
} // namespace

PresetStore& PresetStore::Instance() {
    static PresetStore instance;
    return instance;
}

std::vector<StoredPreset> PresetStore::Get() const {
    std::vector<StoredPreset> out;
    auto doc = DatabaseManager::Instance().Get(kCollection, kKey);
    if (!doc.ok())
        return out;   // nothing saved yet — an empty roster is not an error

    const json::Value::Array* arr = doc.value().asArray();
    if (!arr)
        return out;   // corrupt shape: start fresh rather than fail the boot

    out.reserve(arr->size());
    for (const json::Value& v : *arr) {
        if (auto p = PresetFromJson(v); p.ok())
            out.push_back(std::move(p.value()));
    }
    return out;
}

Result<void> PresetStore::Save(const std::vector<StoredPreset>& presets) const {
    json::Value::Array arr;
    arr.reserve(presets.size());
    for (const StoredPreset& p : presets)
        arr.push_back(PresetToJson(p));
    auto r = DatabaseManager::Instance().Put(kCollection, kKey, json::Value(std::move(arr)));
    if (!r.ok())
        return r;
    // Put() only updates memory; Flush() writes the file — without it a crash
    // right after saving loses the roster (same contract GroupStore::Save follows).
    return DatabaseManager::Instance().Flush();
}

} // namespace bps::project
