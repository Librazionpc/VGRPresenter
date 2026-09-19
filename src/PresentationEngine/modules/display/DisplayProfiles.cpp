#include "modules/display/DisplayProfiles.hpp"

#include <algorithm>

namespace bps::display {

namespace {

json::Value OutputToJson(const Output& o) {
    json::Value::Object obj;
    obj["id"] = json::Value::String(o.id);
    obj["name"] = json::Value::String(o.name);
    obj["kind"] = json::Value::Number(static_cast<double>(static_cast<int>(o.kind)));
    obj["displayId"] = json::Value::String(o.displayId);
    obj["enabled"] = json::Value::Bool(o.enabled);
    obj["autoRestore"] = json::Value::Bool(o.autoRestore);
    obj["scaling"] = json::Value::Number(static_cast<double>(static_cast<int>(o.transform.scaling)));
    obj["color"] = json::Value::Number(static_cast<double>(static_cast<int>(o.transform.color)));
    obj["x"] = json::Value::Number(static_cast<double>(o.transform.x));
    obj["y"] = json::Value::Number(static_cast<double>(o.transform.y));
    obj["width"] = json::Value::Number(static_cast<double>(o.transform.width));
    obj["height"] = json::Value::Number(static_cast<double>(o.transform.height));
    if (o.transform.sourceRect.width > 0 && o.transform.sourceRect.height > 0) {
        json::Value::Object sr;
        sr["x"] = json::Value::Number(static_cast<double>(o.transform.sourceRect.x));
        sr["y"] = json::Value::Number(static_cast<double>(o.transform.sourceRect.y));
        sr["w"] = json::Value::Number(static_cast<double>(o.transform.sourceRect.width));
        sr["h"] = json::Value::Number(static_cast<double>(o.transform.sourceRect.height));
        obj["sourceRect"] = json::Value(std::move(sr));
    }
    return json::Value(std::move(obj));
}

Output OutputFromJson(const json::Value& v) {
    Output o;
    o.id = std::string(v.Find("id") ? v.Find("id")->asString() : "");
    o.name = std::string(v.Find("name") ? v.Find("name")->asString() : "");
    o.kind = static_cast<OutputKind>(v.Find("kind") ? v.Find("kind")->asInt() : 0);
    o.displayId = std::string(v.Find("displayId") ? v.Find("displayId")->asString() : "");
    o.enabled = v.Find("enabled") ? v.Find("enabled")->asBool(true) : true;
    o.autoRestore = v.Find("autoRestore") ? v.Find("autoRestore")->asBool(true) : true;
    o.transform.scaling = static_cast<ScalingMode>(v.Find("scaling") ? v.Find("scaling")->asInt() : 1);
    o.transform.color = static_cast<ColorProfile>(v.Find("color") ? v.Find("color")->asInt() : 0);
    o.transform.x = v.Find("x") ? static_cast<int>(v.Find("x")->asInt()) : 0;
    o.transform.y = v.Find("y") ? static_cast<int>(v.Find("y")->asInt()) : 0;
    o.transform.width = v.Find("width") ? static_cast<int>(v.Find("width")->asInt()) : 0;
    o.transform.height = v.Find("height") ? static_cast<int>(v.Find("height")->asInt()) : 0;
    if (const auto* sr = v.Find("sourceRect")) {
        o.transform.sourceRect = rendering::Rect(
            static_cast<float>(sr->Find("x") ? sr->Find("x")->asNumber() : 0.0),
            static_cast<float>(sr->Find("y") ? sr->Find("y")->asNumber() : 0.0),
            static_cast<float>(sr->Find("w") ? sr->Find("w")->asNumber() : 0.0),
            static_cast<float>(sr->Find("h") ? sr->Find("h")->asNumber() : 0.0));
    }
    return o;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> DisplayProfileStore::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return Ok();
}

Result<void> DisplayProfileStore::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.clear();
    initialized_ = false;
    return Ok();
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
Result<void> DisplayProfileStore::SaveProfile(const DisplayProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_[profile.name] = profile;
    return Ok();
}

Result<void> DisplayProfileStore::DeleteProfile(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = profiles_.find(name);
    if (it == profiles_.end())
        return Error::Make(Err::Display_ProfileNotFound, "DisplayProfileStore",
                           "profile '" + std::string(name) + "' not found");
    profiles_.erase(it);
    return Ok();
}

Result<DisplayProfile> DisplayProfileStore::GetProfile(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = profiles_.find(name);
    if (it == profiles_.end())
        return Error::Make(Err::Display_ProfileNotFound, "DisplayProfileStore",
                           "profile '" + std::string(name) + "' not found");
    return it->second;
}

std::vector<std::string> DisplayProfileStore::ProfileNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (const auto& [name, _] : profiles_) names.push_back(name);
    return names;
}

size_t DisplayProfileStore::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_.size();
}

void DisplayProfileStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.clear();
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
std::string DisplayProfileStore::ToJson(const DisplayProfile& profile) {
    json::Value::Object root;
    root["name"] = json::Value::String(profile.name);
    json::Value::Array outputs;
    outputs.reserve(profile.outputs.size());
    for (const auto& o : profile.outputs) outputs.push_back(OutputToJson(o));
    root["outputs"] = json::Value(std::move(outputs));
    json::Value::Array layout;
    layout.reserve(profile.layout.size());
    for (const auto& item : profile.layout) {
        json::Value::Object li;
        li["outputId"] = json::Value::String(item.outputId);
        li["x"] = json::Value::Number(static_cast<double>(item.x));
        li["y"] = json::Value::Number(static_cast<double>(item.y));
        li["width"] = json::Value::Number(static_cast<double>(item.width));
        li["height"] = json::Value::Number(static_cast<double>(item.height));
        layout.push_back(json::Value(std::move(li)));
    }
    root["layout"] = json::Value(std::move(layout));
    return json::Value(std::move(root)).ToString();
}

Result<DisplayProfile> DisplayProfileStore::FromJson(std::string_view json) {
    auto rootResult = json::Parse(json);
    if (!rootResult.ok()) return rootResult.error();
    const json::Value& root = rootResult.value();
    const auto* obj = root.asObject();
    if (!obj) return Error::Make(Err::Display_Unsupported, "DisplayProfileStore",
                                 "profile JSON must be an object");

    DisplayProfile profile;
    profile.name = std::string(root.Find("name") ? root.Find("name")->asString() : "");
    if (const auto* outputs = root.Find("outputs") ? root.Find("outputs")->asArray() : nullptr) {
        for (const auto& v : *outputs) profile.outputs.push_back(OutputFromJson(v));
    }
    if (const auto* layout = root.Find("layout") ? root.Find("layout")->asArray() : nullptr) {
        for (const auto& v : *layout) {
            LayoutItem item;
            item.outputId = std::string(v.Find("outputId") ? v.Find("outputId")->asString() : "");
            item.x = v.Find("x") ? static_cast<int>(v.Find("x")->asInt()) : 0;
            item.y = v.Find("y") ? static_cast<int>(v.Find("y")->asInt()) : 0;
            item.width = v.Find("width") ? static_cast<int>(v.Find("width")->asInt()) : 0;
            item.height = v.Find("height") ? static_cast<int>(v.Find("height")->asInt()) : 0;
            profile.layout.push_back(item);
        }
    }
    return profile;
}

} // namespace bps::display
