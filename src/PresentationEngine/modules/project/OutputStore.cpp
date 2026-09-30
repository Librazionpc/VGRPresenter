#include "modules/project/OutputStore.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/logging/Logger.hpp"

namespace bps::project {

namespace {
// Collection + key inside the kernel's DatabaseManager document store
// (<dataDir>/kernel.json) — the same document the StyleStore writes its
// roster into, one collection over.
constexpr const char* kCollection = "outputs";
constexpr const char* kKey = "roster";
// The Edit dialog's item-kind keys, in StoredOutput::contentToggles order.
constexpr const char* kContentKeys[6] = { "text", "camera", "media", "clock", "timer", "shape" };
} // namespace

OutputStore& OutputStore::Instance() {
    static OutputStore instance;
    return instance;
}

std::vector<StoredOutput> OutputStore::Get() const {
    std::vector<StoredOutput> out;
    auto doc = DatabaseManager::Instance().Get(kCollection, kKey);
    if (!doc.ok())
        return out;   // nothing saved yet — an empty roster is not an error

    const json::Value::Array* arr = doc.value().asArray();
    if (!arr)
        return out;   // corrupt shape: start fresh rather than fail the boot

    out.reserve(arr->size());
    for (const json::Value& v : *arr) {
        if (auto o = OutputFromJson(v); o.ok())
            out.push_back(std::move(o.value()));
    }
    return out;
}

Result<void> OutputStore::Save(const std::vector<StoredOutput>& outputs) {
    json::Value::Array arr;
    arr.reserve(outputs.size());
    for (const StoredOutput& o : outputs)
        arr.push_back(OutputToJson(o));
    auto r = DatabaseManager::Instance().Put(kCollection, kKey, json::Value(std::move(arr)));
    if (!r.ok())
        return r;
    // Put() only updates memory; Flush() writes the file (same contract as
    // StyleStore::Save — a crash must not eat the roster).
    return DatabaseManager::Instance().Flush();
}

json::Value OutputStore::OutputToJson(const StoredOutput& o) {
    json::Value::Object c;
    for (int i = 0; i < 6; ++i)
        c[kContentKeys[i]] = json::Value::Bool(o.contentToggles[i]);

    json::Value::Object obj;
    obj["id"] = json::Value::String(o.id);
    obj["name"] = json::Value::String(o.name);
    obj["badge"] = json::Value::String(o.badge);
    obj["kind"] = json::Value::String(o.kind);
    obj["res"] = json::Value::String(o.res);
    obj["refresh"] = json::Value::String(o.refresh);
    obj["testPattern"] = json::Value::String(o.testPattern);
    obj["screenName"] = json::Value::String(o.screenName);
    obj["boundsLocked"] = json::Value::Bool(o.boundsLocked);
    obj["stayOnTop"] = json::Value::Bool(o.stayOnTop);
    obj["fullscreenOutput"] = json::Value::Bool(o.fullscreenOutput);
    obj["active"] = json::Value::Bool(o.active);
    obj["enabled"] = json::Value::Bool(o.enabled);
    obj["styleId"] = json::Value::String(o.styleId);
    obj["content"] = json::Value(std::move(c));
    obj["category"] = json::Value::String(o.category);
    return json::Value(std::move(obj));
}

Result<StoredOutput> OutputStore::OutputFromJson(const json::Value& v) {
    const json::Value::Object* o = v.asObject();
    if (!o)
        return Error::Make(Err::InvalidArgument, "OutputStore", "output entry is not an object");

    // GUARDED READS: Find() returns nullptr for a missing key — every field
    // an OLDER saved roster may not carry goes through a defaulted read, or
    // hydrating a pre-existing kernel.json null-derefs.
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

    StoredOutput out;
    out.id = str("id");
    out.name = str("name");
    out.badge = str("badge");
    out.kind = str("kind", "HDMI");
    out.res = str("res");
    out.refresh = str("refresh");
    out.testPattern = str("testPattern", "none");
    out.screenName = str("screenName");
    out.boundsLocked = flag("boundsLocked", false);
    out.stayOnTop = flag("stayOnTop", false);
    out.fullscreenOutput = flag("fullscreenOutput", true);
    out.active = flag("active", false);
    out.enabled = flag("enabled", true);
    out.styleId = str("styleId");
    out.category = str("category");
    if (const json::Value* c = v.Find("content"); c && c->asObject()) {
        // Reads go through the NESTED content object (the outer flag() above
        // would silently return defaults for keys that live one level down).
        const auto tflag = [c](std::string_view key, bool dflt) {
            if (const json::Value* f = c->Find(key))
                return f->asBool(dflt);
            return dflt;
        };
        for (int i = 0; i < 6; ++i)
            out.contentToggles[i] = tflag(kContentKeys[i], true);
    }

    // An entry without an id (hand-edited file, partial write) is skipped by
    // the caller — ids are the stable identity style references key on.
    if (out.id.empty())
        return Error::Make(Err::InvalidArgument, "OutputStore", "output entry has no id");
    return out;
}

} // namespace bps::project
