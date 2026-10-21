#include "modules/project/ActionStore.hpp"

#include "core/database/DatabaseManager.hpp"

namespace bps::project {

namespace {
constexpr const char* kCollection = "actions";
constexpr const char* kKey = "roster";

json::Value ActionToJson(const StoredAction& a) {
    json::Value::Object o;
    o["id"] = json::Value::String(a.id);
    o["name"] = json::Value::String(a.name);
    o["icon"] = json::Value::String(a.icon);
    o["effectId"] = json::Value::String(a.effectId);
    o["effectValue"] = json::Value::String(a.effectValue);
    o["shortcut"] = json::Value::String(a.shortcut);
    o["trigger"] = json::Value::String(a.trigger);
    o["visibleInFunctions"] = json::Value::Bool(a.visibleInFunctions);
    json::Value::Array steps;
    steps.reserve(a.steps.size());
    for (const auto& step : a.steps) {
        json::Value::Object item;
        item["effectId"] = json::Value::String(step.effectId);
        item["effectValue"] = json::Value::String(step.effectValue);
        steps.emplace_back(std::move(item));
    }
    o["steps"] = json::Value(std::move(steps));
    return json::Value(std::move(o));
}

Result<StoredAction> ActionFromJson(const json::Value& v) {
    const json::Value::Object* o = v.asObject();
    if (!o)
        return Error::Make(Err::InvalidArgument, "ActionStore", "action entry is not an object");

    const auto str = [&v](std::string_view key, std::string_view dflt = {}) {
        if (const json::Value* f = v.Find(key))
            return std::string(f->asString(dflt));
        return std::string(dflt);
    };

    StoredAction a;
    a.id = str("id");
    a.name = str("name");
    a.icon = str("icon", "flag");
    a.effectId = str("effectId");
    a.effectValue = str("effectValue");
    a.shortcut = str("shortcut");
    a.trigger = str("trigger");
    if (const json::Value* visible = v.Find("visibleInFunctions"))
        a.visibleInFunctions = visible->asBool(true);
    if (const json::Value* storedSteps = v.Find("steps")) {
        if (const json::Value::Array* array = storedSteps->asArray()) {
            for (const json::Value& entry : *array) {
                if (!entry.asObject())
                    continue;
                const auto stepString = [&entry](std::string_view key) {
                    if (const json::Value* field = entry.Find(key))
                        return std::string(field->asString());
                    return std::string{};
                };
                const std::string effectId = stepString("effectId");
                if (!effectId.empty())
                    a.steps.push_back({ effectId, stepString("effectValue") });
            }
        }
    }
    // Read 0.0.8 and older action files, which stored exactly one operation.
    if (a.steps.empty() && !a.effectId.empty())
        a.steps.push_back({ a.effectId, a.effectValue });
    if (!a.steps.empty()) {
        a.effectId = a.steps.front().effectId;
        a.effectValue = a.steps.front().effectValue;
    }
    return a;
}
} // namespace

ActionStore& ActionStore::Instance() {
    static ActionStore instance;
    return instance;
}

std::vector<StoredAction> ActionStore::Get() const {
    std::vector<StoredAction> out;
    auto doc = DatabaseManager::Instance().Get(kCollection, kKey);
    if (!doc.ok())
        return out;

    const json::Value::Array* arr = doc.value().asArray();
    if (!arr)
        return out;

    out.reserve(arr->size());
    for (const json::Value& v : *arr) {
        if (auto a = ActionFromJson(v); a.ok())
            out.push_back(std::move(a.value()));
    }
    return out;
}

Result<void> ActionStore::Save(const std::vector<StoredAction>& actions) const {
    json::Value::Array arr;
    arr.reserve(actions.size());
    for (const StoredAction& a : actions)
        arr.push_back(ActionToJson(a));
    auto r = DatabaseManager::Instance().Put(kCollection, kKey, json::Value(std::move(arr)));
    if (!r.ok())
        return r;
    return DatabaseManager::Instance().Flush();
}

} // namespace bps::project
