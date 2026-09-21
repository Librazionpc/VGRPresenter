#include "modules/presentation/PresentationSerializer.hpp"

#include "core/config/Json.hpp"

#include <chrono>
#include <format>

namespace bps::presentation {

namespace {

namespace js = bps::json;

constexpr const char* kModule = "PresentationSerializer";

int64_t ToMs(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromMs(int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

js::Value Str(std::string_view s) { return js::Value::String(std::string(s)); }

js::Value StringArray(const std::vector<std::string>& in) {
    js::Value::Array arr;
    arr.reserve(in.size());
    for (const auto& s : in) arr.push_back(Str(s));
    return js::Value(std::move(arr));
}

// Field readers: a wrong-typed or absent field falls back to the default, so a
// hand-edited or newer file degrades instead of failing to open.
std::string GetStr(const js::Value& obj, std::string_view key, std::string dflt = {}) {
    const js::Value* v = obj.Find(key);
    return v && v->type() == js::Value::Type::String ? std::string(v->asString()) : dflt;
}
double GetNum(const js::Value& obj, std::string_view key, double dflt) {
    const js::Value* v = obj.Find(key);
    return v && v->type() == js::Value::Type::Number ? v->asNumber() : dflt;
}
bool GetBool(const js::Value& obj, std::string_view key, bool dflt) {
    const js::Value* v = obj.Find(key);
    return v && v->type() == js::Value::Type::Bool ? v->asBool() : dflt;
}
std::vector<std::string> GetStrings(const js::Value& obj, std::string_view key) {
    std::vector<std::string> out;
    const js::Value* v = obj.Find(key);
    if (!v || !v->asArray()) return out;
    for (const auto& e : *v->asArray())
        if (e.type() == js::Value::Type::String) out.emplace_back(e.asString());
    return out;
}

// metaJson (an object string) <-> a real nested JSON object in the file.
js::Value MetaToValue(const std::string& metaJson) {
    if (auto parsed = js::Parse(metaJson); parsed.ok() && parsed.value().asObject())
        return parsed.value();
    return js::Value(js::Value::Object{});
}
std::string MetaFromValue(const js::Value& obj, std::string_view key) {
    const js::Value* meta = obj.Find(key);
    return meta && meta->asObject() ? meta->ToString() : std::string("{}");
}

js::Value BlockToJson(const ContentBlock& b) {
    js::Value::Object style;
    style["padding"] = js::Value::Number(b.style.padding);
    style["backgroundColor"] = Str(b.style.backgroundColor);
    style["cornerRadius"] = js::Value::Number(b.style.cornerRadius);
    style["borderEnabled"] = js::Value::Bool(b.style.borderEnabled);
    style["borderWidth"] = js::Value::Number(b.style.borderWidth);
    style["borderStyle"] = Str(b.style.borderStyle);
    style["borderColor"] = Str(b.style.borderColor);

    js::Value::Object o;
    o["id"] = Str(b.id);
    o["kind"] = Str(b.kind);
    o["text"] = Str(b.text);
    o["x"] = js::Value::Number(b.x);
    o["y"] = js::Value::Number(b.y);
    o["width"] = js::Value::Number(b.width);
    o["height"] = js::Value::Number(b.height);
    if (!b.bind.empty()) o["bind"] = Str(b.bind);
    // meta is stored as a real JSON object (readable in the file), not a string
    // of JSON. An unparsable/non-object metaJson degrades to {}.
    js::Value meta = js::Value(js::Value::Object{});
    if (auto parsed = js::Parse(b.metaJson); parsed.ok() && parsed.value().asObject())
        meta = parsed.value();
    o["meta"] = std::move(meta);
    o["style"] = js::Value(std::move(style));
    return js::Value(std::move(o));
}

ContentBlock BlockFromJson(const js::Value& o, size_t index) {
    ContentBlock b;
    b.id = GetStr(o, "id");
    if (b.id.empty()) b.id = std::format("block-{}", index + 1);
    b.kind = GetStr(o, "kind", "text");
    b.text = GetStr(o, "text");
    b.x = GetNum(o, "x", 0);
    b.y = GetNum(o, "y", 0);
    b.width = GetNum(o, "width", 160);
    b.height = GetNum(o, "height", 40);
    b.bind = GetStr(o, "bind");
    if (const js::Value* meta = o.Find("meta"); meta && meta->asObject())
        b.metaJson = meta->ToString();
    if (const js::Value* s = o.Find("style"); s && s->asObject()) {
        b.style.padding = GetNum(*s, "padding", 0);
        b.style.backgroundColor = GetStr(*s, "backgroundColor", "transparent");
        b.style.cornerRadius = GetNum(*s, "cornerRadius", 0);
        b.style.borderEnabled = GetBool(*s, "borderEnabled", false);
        b.style.borderWidth = GetNum(*s, "borderWidth", 2);
        b.style.borderStyle = GetStr(*s, "borderStyle", "line");
        b.style.borderColor = GetStr(*s, "borderColor", "#ffffff");
    }
    return b;
}

js::Value SlideToJson(const Slide& s) {
    js::Value::Array blocks;
    blocks.reserve(s.blocks.size());
    for (const auto& b : s.blocks) blocks.push_back(BlockToJson(b));

    js::Value::Object o;
    o["id"] = Str(s.id);
    o["title"] = Str(s.title);
    o["text"] = Str(s.text);
    o["notes"] = Str(s.notes);
    o["tags"] = StringArray(s.tags);
    o["sections"] = StringArray(s.sections);
    o["assetIds"] = StringArray(s.assetIds);
    o["hidden"] = js::Value::Bool(s.hidden);
    o["transitionIn"] = js::Value::Number(static_cast<int>(s.transitionIn));
    o["transitionMs"] = js::Value::Number(s.transitionMs);
    o["durationMs"] = js::Value::Number(s.durationMs);
    o["background"] = Str(s.background);
    if (!s.categoryId.empty()) o["categoryId"] = Str(s.categoryId);
    o["meta"] = MetaToValue(s.metaJson);
    o["blocks"] = js::Value(std::move(blocks));
    return js::Value(std::move(o));
}

Slide SlideFromJson(const js::Value& o, size_t index) {
    Slide s;
    s.id = GetStr(o, "id");
    if (s.id.empty()) s.id = std::format("slide-{}", index + 1);
    s.title = GetStr(o, "title");
    s.text = GetStr(o, "text");
    s.notes = GetStr(o, "notes");
    s.tags = GetStrings(o, "tags");
    s.sections = GetStrings(o, "sections");
    s.assetIds = GetStrings(o, "assetIds");
    s.hidden = GetBool(o, "hidden", false);
    s.transitionIn = static_cast<TransitionKind>(
        static_cast<int>(GetNum(o, "transitionIn", static_cast<double>(TransitionKind::Fade))));
    s.transitionMs = GetNum(o, "transitionMs", 500.0);
    s.durationMs = GetNum(o, "durationMs", 0.0);
    s.background = GetStr(o, "background", "transparent");
    s.categoryId = GetStr(o, "categoryId");
    s.metaJson = MetaFromValue(o, "meta");
    if (const js::Value* blocks = o.Find("blocks"); blocks && blocks->asArray()) {
        size_t i = 0;
        for (const auto& b : *blocks->asArray()) {
            if (b.asObject()) s.blocks.push_back(BlockFromJson(b, i));
            ++i;
        }
    }
    return s;
}

js::Value BlocksToJson(const std::vector<ContentBlock>& blocks) {
    js::Value::Array arr;
    arr.reserve(blocks.size());
    for (const auto& b : blocks) arr.push_back(BlockToJson(b));
    return js::Value(std::move(arr));
}

std::vector<ContentBlock> BlocksFromJson(const js::Value& o, std::string_view key) {
    std::vector<ContentBlock> out;
    if (const js::Value* blocks = o.Find(key); blocks && blocks->asArray()) {
        size_t i = 0;
        for (const auto& b : *blocks->asArray()) {
            if (b.asObject()) out.push_back(BlockFromJson(b, i));
            ++i;
        }
    }
    return out;
}

js::Value CategoryToJson(const Category& c) {
    js::Value::Object o;
    o["id"] = Str(c.id);
    o["name"] = Str(c.name);
    o["contentType"] = Str(c.contentType);
    o["templateId"] = Str(c.templateId);
    o["outputs"] = StringArray(c.outputs);
    o["meta"] = MetaToValue(c.metaJson);
    return js::Value(std::move(o));
}

Category CategoryFromJson(const js::Value& o, size_t index) {
    Category c;
    c.id = GetStr(o, "id");
    if (c.id.empty()) c.id = std::format("category-{}", index + 1);
    c.name = GetStr(o, "name");
    c.contentType = GetStr(o, "contentType");
    c.templateId = GetStr(o, "templateId");
    c.outputs = GetStrings(o, "outputs");
    c.metaJson = MetaFromValue(o, "meta");
    return c;
}

js::Value TemplateToJson(const SlideTemplate& t) {
    js::Value::Object o;
    o["id"] = Str(t.id);
    o["name"] = Str(t.name);
    o["contentType"] = Str(t.contentType);
    o["background"] = Str(t.background);
    o["meta"] = MetaToValue(t.metaJson);
    o["blocks"] = BlocksToJson(t.blocks);
    return js::Value(std::move(o));
}

SlideTemplate TemplateFromJson(const js::Value& o, size_t index) {
    SlideTemplate t;
    t.id = GetStr(o, "id");
    if (t.id.empty()) t.id = std::format("template-{}", index + 1);
    t.name = GetStr(o, "name");
    t.contentType = GetStr(o, "contentType");
    t.background = GetStr(o, "background", "transparent");
    t.metaJson = MetaFromValue(o, "meta");
    t.blocks = BlocksFromJson(o, "blocks");
    return t;
}

js::Value OverlayToJson(const Overlay& v) {
    js::Value::Object o;
    o["id"] = Str(v.id);
    o["name"] = Str(v.name);
    o["scope"] = js::Value::Number(static_cast<int>(v.scope));
    o["targetId"] = Str(v.targetId);
    o["outputs"] = StringArray(v.outputs);
    o["enabled"] = js::Value::Bool(v.enabled);
    o["meta"] = MetaToValue(v.metaJson);
    o["blocks"] = BlocksToJson(v.blocks);
    return js::Value(std::move(o));
}

Overlay OverlayFromJson(const js::Value& o, size_t index) {
    Overlay v;
    v.id = GetStr(o, "id");
    if (v.id.empty()) v.id = std::format("overlay-{}", index + 1);
    v.name = GetStr(o, "name");
    const int scope = static_cast<int>(GetNum(o, "scope", 0));
    v.scope = scope >= 0 && scope <= static_cast<int>(OverlayScope::Slide)
                  ? static_cast<OverlayScope>(scope) : OverlayScope::All;
    v.targetId = GetStr(o, "targetId");
    v.outputs = GetStrings(o, "outputs");
    v.enabled = GetBool(o, "enabled", true);
    v.metaJson = MetaFromValue(o, "meta");
    v.blocks = BlocksFromJson(o, "blocks");
    return v;
}

// Reads an optional array-of-objects field with `fn`; a member that is not an
// object (or a field that is not an array) is an error.
template <typename T, typename Fn>
Result<std::vector<T>> ReadObjects(const js::Value& root, std::string_view key, Fn fn) {
    std::vector<T> out;
    const js::Value* arr = root.Find(key);
    if (!arr) return out;
    if (!arr->asArray())
        return Error::Make(Err::InvalidArgument, kModule, std::format("\"{}\" must be an array", key));
    size_t i = 0;
    for (const auto& e : *arr->asArray()) {
        if (!e.asObject())
            return Error::Make(Err::InvalidArgument, kModule,
                               std::format("{} entry {} is not an object", key, i + 1));
        out.push_back(fn(e, i));
        ++i;
    }
    return out;
}

} // namespace

Result<std::string> PresentationSerializer::SerializeTemplate(const SlideTemplate& t) const {
    js::Value::Object root;
    root["schemaVersion"] = js::Value::Number(kSchemaVersion);
    root["template"] = TemplateToJson(t);
    return js::Value(std::move(root)).ToString();
}

Result<SlideTemplate> PresentationSerializer::DeserializeTemplate(std::string_view json) const {
    auto parsed = js::Parse(json);
    if (!parsed.ok())
        return Error::Make(Err::InvalidArgument, kModule,
                           "template JSON is malformed: " + parsed.error().message);
    const js::Value& root = parsed.value();
    const js::Value* ver = root.Find("schemaVersion");
    if (!ver || ver->type() != js::Value::Type::Number)
        return Error::Make(Err::InvalidArgument, kModule, "template JSON has no schemaVersion");
    if (ver->asInt() > kSchemaVersion)
        return Error::Make(Err::InvalidArgument, kModule, "template schema is newer than this engine");
    const js::Value* t = root.Find("template");
    if (!t || !t->asObject())
        return Error::Make(Err::InvalidArgument, kModule, "template JSON has no \"template\" object");
    return TemplateFromJson(*t, 0);
}

Result<std::string> PresentationSerializer::Serialize(const Presentation& p) const {
    js::Value::Array slides;
    slides.reserve(p.slides.size());
    for (const auto& s : p.slides) slides.push_back(SlideToJson(s));

    js::Value::Array categories, templates, overlays;
    for (const auto& c : p.categories) categories.push_back(CategoryToJson(c));
    for (const auto& t : p.templates) templates.push_back(TemplateToJson(t));
    for (const auto& v : p.overlays) overlays.push_back(OverlayToJson(v));

    js::Value::Object root;
    root["schemaVersion"] = js::Value::Number(kSchemaVersion);
    root["id"] = Str(p.id);
    root["name"] = Str(p.name);
    root["path"] = Str(p.path);
    root["playbackMode"] = js::Value::Number(static_cast<int>(p.mode));
    root["defaultTransitionMs"] = js::Value::Number(p.defaultTransitionMs);
    root["createdAtMs"] = js::Value::Number(static_cast<double>(ToMs(p.createdAt)));
    root["modifiedAtMs"] = js::Value::Number(static_cast<double>(ToMs(p.modifiedAt)));
    root["categories"] = js::Value(std::move(categories));
    root["templates"] = js::Value(std::move(templates));
    root["overlays"] = js::Value(std::move(overlays));
    root["meta"] = MetaToValue(p.metaJson);
    root["slides"] = js::Value(std::move(slides));
    return js::Value(std::move(root)).ToString();
}

Result<Presentation> PresentationSerializer::Deserialize(std::string_view json) const {
    auto parsed = js::Parse(json);
    if (!parsed.ok())
        return Error::Make(Err::InvalidArgument, kModule,
                           "presentation JSON is malformed: " + parsed.error().message);
    const js::Value& root = parsed.value();
    if (!root.asObject())
        return Error::Make(Err::InvalidArgument, kModule, "presentation JSON must be an object");

    const js::Value* ver = root.Find("schemaVersion");
    if (!ver || ver->type() != js::Value::Type::Number)
        return Error::Make(Err::InvalidArgument, kModule, "presentation JSON has no schemaVersion");
    if (ver->asInt() > kSchemaVersion)
        return Error::Make(Err::InvalidArgument, kModule,
                           std::format("presentation schema v{} is newer than this engine understands (v{})",
                                       ver->asInt(), kSchemaVersion));

    Presentation p;
    p.id = GetStr(root, "id");
    p.name = GetStr(root, "name");
    p.path = GetStr(root, "path");
    p.mode = static_cast<PlaybackMode>(static_cast<int>(GetNum(root, "playbackMode", 0)));
    p.defaultTransitionMs = GetNum(root, "defaultTransitionMs", 500.0);
    p.createdAt = FromMs(static_cast<int64_t>(GetNum(root, "createdAtMs", 0)));
    p.modifiedAt = FromMs(static_cast<int64_t>(GetNum(root, "modifiedAtMs", 0)));
    p.metaJson = MetaFromValue(root, "meta");

    const js::Value* slides = root.Find("slides");
    if (slides && !slides->asArray())
        return Error::Make(Err::InvalidArgument, kModule, "\"slides\" must be an array");
    if (slides) {
        size_t i = 0;
        for (const auto& s : *slides->asArray()) {
            if (!s.asObject())
                return Error::Make(Err::InvalidArgument, kModule,
                                   std::format("slide {} is not an object", i + 1));
            p.slides.push_back(SlideFromJson(s, i));
            ++i;
        }
    }

    // Show structure — all optional, so shows saved before categories/templates/
    // overlays existed still open.
    auto categories = ReadObjects<Category>(root, "categories", CategoryFromJson);
    if (!categories.ok()) return categories.error();
    p.categories = std::move(categories.value());
    auto templates = ReadObjects<SlideTemplate>(root, "templates", TemplateFromJson);
    if (!templates.ok()) return templates.error();
    p.templates = std::move(templates.value());
    auto overlays = ReadObjects<Overlay>(root, "overlays", OverlayFromJson);
    if (!overlays.ok()) return overlays.error();
    p.overlays = std::move(overlays.value());
    return p;
}

} // namespace bps::presentation
