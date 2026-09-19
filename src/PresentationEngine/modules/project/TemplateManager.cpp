#include "modules/project/TemplateManager.hpp"

#include "modules/project/ProjectManager.hpp"

#include "core/database/DatabaseManager.hpp"

#include <algorithm>
#include <chrono>
#include <format>

namespace bps::project {

namespace {
int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
std::string GenerateId() {
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t v = static_cast<uint64_t>(t) ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 13);
    return std::format("tpl-{:x}", v);
}
} // namespace

TemplateManager& TemplateManager::Instance() {
    static TemplateManager instance;
    return instance;
}

Result<std::string> TemplateManager::Create(std::string_view name, std::string_view sourceProjectId,
                                            std::string_view description) {
    auto src = ProjectManager::Instance().Get(sourceProjectId);
    if (!src.ok()) return src.error();

    ProjectTemplate t;
    t.id = GenerateId();
    t.name = std::string(name);
    t.description = std::string(description);
    t.sourceProjectId = std::string(sourceProjectId);
    t.createdAtMs = MsNow();
    // Capture project settings + asset references (drop volatile fields).
    t.data = ProjectToJson(src.value());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool first = templates_.empty();
        templates_.push_back(t);
        if (first) t.isDefault = true;
    }
    return Result<std::string>{t.id};
}

std::vector<ProjectTemplate> TemplateManager::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return templates_;
}

Result<ProjectTemplate> TemplateManager::Get(std::string_view templateId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(templates_.begin(), templates_.end(),
                           [&](const ProjectTemplate& t) { return t.id == templateId; });
    if (it == templates_.end())
        return Error::Make(Err::NotFound, "Template", "no such template");
    return Result<ProjectTemplate>{*it};
}

Result<void> TemplateManager::Remove(std::string_view templateId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(templates_.begin(), templates_.end(),
                           [&](const ProjectTemplate& t) { return t.id == templateId; });
    if (it == templates_.end())
        return Error::Make(Err::NotFound, "Template", "no such template");
    bool wasDefault = it->isDefault;
    templates_.erase(it);
    if (wasDefault && !templates_.empty()) templates_.front().isDefault = true;
    return Ok();
}

Result<void> TemplateManager::SetDefault(std::string_view templateId) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto& t : templates_) {
        t.isDefault = (t.id == templateId);
        if (t.id == templateId) found = true;
    }
    if (!found) return Error::Make(Err::NotFound, "Template", "no such template");
    return Ok();
}

Result<ProjectTemplate> TemplateManager::Default() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& t : templates_)
        if (t.isDefault) return Result<ProjectTemplate>{t};
    return Error::Make(Err::NotFound, "Template", "no default template");
}

Result<Project> TemplateManager::Apply(std::string_view templateId, std::string_view projectName) {
    auto t = Get(templateId);
    if (!t.ok()) return t.error();
    auto created = ProjectManager::Instance().Create(projectName, t.value().id);
    if (!created.ok()) return created.error();
    // Seed project settings from the template (best effort).
    auto p = ProjectFromJson(t.value().data);
    if (p.ok()) {
        created.value().settings = p.value().settings;
        created.value().assetUuids = p.value().assetUuids;
        created.value().description = t.value().description;
        (void)ProjectManager::Instance().Save(created.value().id, false);
    }
    return created;
}

Result<void> TemplateManager::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    json::Value::Array arr;
    for (const auto& t : templates_) {
        json::Value::Object o;
        o["id"] = json::Value::String(t.id);
        o["name"] = json::Value::String(t.name);
        o["description"] = json::Value::String(t.description);
        o["version"] = json::Value::String(t.version.ToString());
        o["sourceProjectId"] = json::Value::String(t.sourceProjectId);
        o["isDefault"] = json::Value::Bool(t.isDefault);
        o["createdAtMs"] = json::Value::Number(static_cast<double>(t.createdAtMs));
        o["data"] = t.data;
        arr.push_back(json::Value(std::move(o)));
    }
    return DatabaseManager::Instance().Put("templates", "items", json::Value(std::move(arr)));
}

Result<void> TemplateManager::Load() {
    auto doc = DatabaseManager::Instance().Get("templates", "items");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    templates_.clear();
    if (doc.value().type() != json::Value::Type::Array) return Ok();
    for (const auto& e : *doc.value().asArray()) {
        ProjectTemplate t;
        if (const auto* n = e.Find("id")) t.id = std::string(n->asString());
        if (const auto* n = e.Find("name")) t.name = std::string(n->asString());
        if (const auto* n = e.Find("description")) t.description = std::string(n->asString());
        if (const auto* n = e.Find("version")) t.version = Version::Parse(n->asString());
        if (const auto* n = e.Find("sourceProjectId")) t.sourceProjectId = std::string(n->asString());
        if (const auto* n = e.Find("isDefault")) t.isDefault = n->asBool(false);
        if (const auto* n = e.Find("createdAtMs")) t.createdAtMs = n->asInt(0);
        if (const auto* n = e.Find("data")) t.data = *n;
        templates_.push_back(t);
    }
    return Ok();
}

size_t TemplateManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return templates_.size();
}

} // namespace bps::project
