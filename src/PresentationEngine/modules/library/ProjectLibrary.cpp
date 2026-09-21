#include "modules/library/ProjectLibrary.hpp"

#include "core/config/Json.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <set>

namespace bps::library {

namespace {

constexpr const char* kModule = "ProjectLibrary";
constexpr size_t kMaxItems = 5000;
constexpr int64_t kFiveDaysMs = 5ll * 24 * 3600 * 1000;
using J = json::Value;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

bool In(std::string_view v, std::initializer_list<std::string_view> list) {
    return std::find(list.begin(), list.end(), v) != list.end();
}

const std::set<std::string, std::less<>>& ItemTypes() {
    static const std::set<std::string, std::less<>> types = { "show", "media", "audio", "overlay", "scripture", "section", "effect", "camera",
                                                              "screen", "ndi", "player", "pdf", "image", "video", "folder" };
    return types;
}

std::string Extension(std::string_view name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext = Lower(std::string(name.substr(dot + 1)));
    return ext;
}

std::string FileNameOf(std::string_view path) {
    const size_t cut = path.find_last_of("/\\");
    std::string name(cut == std::string_view::npos ? path : path.substr(cut + 1));
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

Error Invalid(std::string message) { return Error::Make(Err::InvalidArgument, kModule, std::move(message)); }
Error Missing(std::string_view what, std::string_view id) { return Error::Make(Err::NotFound, kModule, std::format("there is no {} '{}'", what, id)); }

std::string Str(const J& o, std::string_view key, std::string dflt = {}) {
    const J* v = o.Find(key);
    return v && v->type() == J::Type::String ? std::string(v->asString()) : dflt;
}
bool Flag(const J& o, std::string_view key) { const J* v = o.Find(key); return v && v->asBool(); }
int64_t Num(const J& o, std::string_view key) { const J* v = o.Find(key); return v ? static_cast<int64_t>(v->asNumber()) : 0; }

J ItemToJson(const ProjectItem& i) {
    J::Object o;
    o["id"] = J::String(i.id); o["type"] = J::String(i.type); o["ref"] = J::String(i.ref); o["name"] = J::String(i.name);
    if (!i.layout.empty()) o["layout"] = J::String(i.layout);
    if (!i.color.empty()) o["color"] = J::String(i.color);
    if (auto meta = json::Parse(i.metaJson); meta.ok() && meta.value().asObject() && !meta.value().asObject()->empty()) o["meta"] = meta.value();
    return J(std::move(o));
}

} // namespace

// ---------------------------------------------------------------------------
// Drop rules
// ---------------------------------------------------------------------------

bool AcceptsDrop(std::string_view area, std::string_view kind, bool reorder) {
    bool ok = false;
    if (area == "all_slides") ok = In(kind, { "template" });
    else if (area == "slides")
        ok = In(kind, { "media", "player", "urls", "audio", "audio_effect", "overlay", "sound", "effect", "screen", "ndi", "camera", "microphone", "scripture",
                        "category_audio", "audio_stream", "metronome", "show", "global_timer", "variable", "midi", "action" });
    else if (area == "project") ok = In(kind, { "show_drawer", "media", "audio", "audio_effect", "overlay", "player", "scripture", "effect", "screen", "ndi", "camera", "files" });
    else if (area == "overlays" || area == "templates") ok = In(kind, { "slide" });
    else if (area == "edit") ok = In(kind, { "media", "global_timer", "variable" });
    if (ok || !reorder) return ok;

    if (area == "projects") return In(kind, { "folder", "project" });
    if (area == "project") return In(kind, { "show", "media", "audio", "audio_effect", "show_drawer", "player", "action" });
    if (area == "slides")
        return In(kind, { "slide", "group", "global_group", "effect", "screen", "ndi", "camera", "microphone", "media", "player", "urls", "audio", "audio_effect", "show" });
    if (area == "navigation") return In(kind, { "show", "show_drawer", "media", "audio", "audio_effect", "overlay", "template" });
    if (area == "audio_playlist") return In(kind, { "audio" });
    return false;
}

std::string ItemTypeForDrop(std::string_view kind, std::string_view fileName) {
    if (kind == "show_drawer" || kind == "show") return "show";
    if (kind == "audio_effect" || kind == "audio") return "audio";
    if (In(kind, { "overlay", "scripture", "effect", "camera", "screen", "ndi", "player" })) return std::string(kind);
    if (kind == "media" || kind == "files") {
        const std::string ext = Extension(fileName);
        if (In(ext, { "png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "avif", "tif", "tiff" })) return "image";
        if (In(ext, { "mp4", "mov", "mkv", "webm", "avi", "m4v", "wmv", "mpg", "mpeg" })) return "video";
        if (In(ext, { "mp3", "wav", "ogg", "flac", "m4a", "aac", "wma", "opus" })) return "audio";
        if (ext == "pdf") return "pdf";
        return kind == "media" ? "media" : "";   // a dropped file that is none of these is not something a project holds
    }
    return "";
}

// ---------------------------------------------------------------------------
// The library
// ---------------------------------------------------------------------------

ProjectLibrary::ProjectLibrary(std::string storageFile) : storageFile_(std::move(storageFile)) {}

void ProjectLibrary::SetClock(std::function<int64_t()> clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
}

int64_t ProjectLibrary::Now() const {
    if (clock_) return clock_();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void ProjectLibrary::Touch(Project& p) { p.modified = Now(); }

Result<void> ProjectLibrary::Load() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::lock_guard<std::mutex> lock(mutex_);
    projects_.clear();
    folders_.clear();
    if (!fs.Exists(storageFile_)) return {};
    auto text = fs.ReadText(storageFile_);
    if (!text.ok()) return text.error();
    auto parsed = json::Parse(text.value());
    if (!parsed.ok()) return Error::Make(Err::ParseError, kModule, std::format("the projects file is damaged: {}", parsed.error().message));
    const J& root = parsed.value();

    if (const J* list = root.Find("folders"); list && list->asArray())
        for (const J& f : *list->asArray()) {
            ProjectFolder folder{ Str(f, "id"), Str(f, "name"), Str(f, "parent"), Num(f, "created") };
            if (!folder.id.empty() && !folder.name.empty()) folders_.push_back(std::move(folder));
        }
    if (const J* list = root.Find("projects"); list && list->asArray())
        for (const J& p : *list->asArray()) {
            Project project;
            project.id = Str(p, "id"); project.name = Str(p, "name"); project.parent = Str(p, "parent");
            project.created = Num(p, "created"); project.modified = Num(p, "modified"); project.used = Num(p, "used");
            project.archived = Flag(p, "archived"); project.sectionsCollapsed = Flag(p, "sectionsCollapsed"); project.sectionsLocked = Flag(p, "sectionsLocked");
            project.notes = Str(p, "notes");
            if (const J* items = p.Find("items"); items && items->asArray())
                for (const J& i : *items->asArray()) {
                    ProjectItem item;
                    item.id = Str(i, "id"); item.type = Str(i, "type"); item.ref = Str(i, "ref"); item.name = Str(i, "name");
                    item.layout = Str(i, "layout"); item.color = Str(i, "color");
                    if (const J* meta = i.Find("meta"); meta && meta->asObject()) item.metaJson = meta->ToString();
                    if (item.id.empty() || !ItemTypes().contains(item.type) || (item.ref.empty() && item.type != "section")) continue;
                    project.items.push_back(std::move(item));
                }
            if (!project.id.empty() && !project.name.empty()) projects_.push_back(std::move(project));
        }
    // Something filed in a folder that no longer exists is just at the top.
    auto known = [&](const std::string& id) { return id.empty() || std::any_of(folders_.begin(), folders_.end(), [&](const ProjectFolder& f) { return f.id == id; }); };
    for (ProjectFolder& f : folders_) if (!known(f.parent) || f.parent == f.id) f.parent.clear();
    for (Project& p : projects_) if (!known(p.parent)) p.parent.clear();
    return {};
}

Result<void> ProjectLibrary::SaveLocked() const {
    J::Array folders, projects;
    for (const ProjectFolder& f : folders_) {
        J::Object o;
        o["id"] = J::String(f.id); o["name"] = J::String(f.name); o["parent"] = J::String(f.parent); o["created"] = J::Number(static_cast<double>(f.created));
        folders.push_back(J(std::move(o)));
    }
    for (const Project& p : projects_) {
        J::Object o;
        o["id"] = J::String(p.id); o["name"] = J::String(p.name); o["parent"] = J::String(p.parent);
        o["created"] = J::Number(static_cast<double>(p.created)); o["modified"] = J::Number(static_cast<double>(p.modified)); o["used"] = J::Number(static_cast<double>(p.used));
        if (p.archived) o["archived"] = J::Bool(true);
        if (p.sectionsCollapsed) o["sectionsCollapsed"] = J::Bool(true);
        if (p.sectionsLocked) o["sectionsLocked"] = J::Bool(true);
        if (!p.notes.empty()) o["notes"] = J::String(p.notes);
        J::Array items;
        for (const ProjectItem& i : p.items) items.push_back(ItemToJson(i));
        o["items"] = J(std::move(items));
        projects.push_back(J(std::move(o)));
    }
    J::Object root;
    root["version"] = J::Number(1);
    root["folders"] = J(std::move(folders));
    root["projects"] = J(std::move(projects));

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const size_t cut = storageFile_.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0)
        if (auto made = fs.CreateDirectories(std::string_view(storageFile_).substr(0, cut)); !made.ok()) return made.error();
    return fs.Write(storageFile_, J(std::move(root)).ToString());
}

Project* ProjectLibrary::FindProject(std::string_view id) {
    for (Project& p : projects_) if (p.id == id) return &p;
    return nullptr;
}
ProjectFolder* ProjectLibrary::FindFolder(std::string_view id) {
    for (ProjectFolder& f : folders_) if (f.id == id) return &f;
    return nullptr;
}

bool ProjectLibrary::IsInside(std::string_view folder, std::string_view ancestor) const {
    std::string at(folder);
    for (size_t hops = 0; !at.empty() && hops <= folders_.size(); ++hops) {
        if (at == ancestor) return true;
        auto it = std::find_if(folders_.begin(), folders_.end(), [&](const ProjectFolder& f) { return f.id == at; });
        if (it == folders_.end()) return false;
        at = it->parent;
    }
    return false;
}

// "<prefix>-<n>" one past the largest used, so an id is never a live one.
std::string ProjectLibrary::NextId(std::string_view prefix) const {
    const std::string lead = std::string(prefix) + "-";
    long long highest = 0;
    auto scan = [&](const std::string& id) {
        if (id.rfind(lead, 0) != 0) return;
        const std::string tail = id.substr(lead.size());
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c); })) highest = std::max(highest, std::stoll(tail));
    };
    for (const Project& p : projects_) scan(p.id);
    for (const ProjectFolder& f : folders_) scan(f.id);
    return lead + std::to_string(highest + 1);
}

// ---------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------

std::vector<ProjectTreeRow> ProjectLibrary::Tree() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProjectTreeRow> rows;
    auto byName = [](const auto& a, const auto& b) { return Lower(a.name) < Lower(b.name); };
    std::function<void(const std::string&, int)> walk = [&](const std::string& parent, int depth) {
        std::vector<const ProjectFolder*> folders;
        for (const ProjectFolder& f : folders_) if (f.parent == parent) folders.push_back(&f);
        std::sort(folders.begin(), folders.end(), [&](auto* a, auto* b) { return byName(*a, *b); });
        for (const ProjectFolder* f : folders) {
            size_t children = 0;
            for (const ProjectFolder& c : folders_) if (c.parent == f->id) ++children;
            for (const Project& c : projects_) if (c.parent == f->id) ++children;
            rows.push_back({ f->id, "folder", f->name, f->parent, depth, children, false });
            walk(f->id, depth + 1);
        }
        std::vector<const Project*> projects;
        for (const Project& p : projects_) if (p.parent == parent) projects.push_back(&p);
        std::stable_sort(projects.begin(), projects.end(), [&](auto* a, auto* b) { return a->archived != b->archived ? !a->archived : byName(*a, *b); });
        for (const Project* p : projects) rows.push_back({ p->id, "project", p->name, p->parent, depth, p->items.size(), p->archived });
    };
    walk("", 0);
    return rows;
}

std::vector<Project> ProjectLibrary::Projects() const { std::lock_guard<std::mutex> lock(mutex_); return projects_; }
std::vector<ProjectFolder> ProjectLibrary::Folders() const { std::lock_guard<std::mutex> lock(mutex_); return folders_; }

Result<Project> ProjectLibrary::Get(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Project& p : projects_) if (p.id == id) return p;
    return Missing("project", id);
}

Result<std::string> ProjectLibrary::Create(std::string_view name, std::string_view parentFolder) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!parentFolder.empty() && !FindFolder(parentFolder)) return Missing("folder", parentFolder);
    Project p;
    p.id = NextId("project");
    p.name = Trim(name).empty() ? std::string("New project") : Trim(name).substr(0, 200);
    p.parent = std::string(parentFolder);
    p.created = p.modified = p.used = Now();
    const std::string id = p.id;
    projects_.push_back(std::move(p));
    if (auto saved = SaveLocked(); !saved.ok()) { projects_.pop_back(); return saved.error(); }
    return id;
}

Result<std::string> ProjectLibrary::CreateFolder(std::string_view name, std::string_view parentFolder) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!parentFolder.empty() && !FindFolder(parentFolder)) return Missing("folder", parentFolder);
    ProjectFolder f;
    f.id = NextId("folder");
    f.name = Trim(name).empty() ? std::string("New folder") : Trim(name).substr(0, 200);
    f.parent = std::string(parentFolder);
    f.created = Now();
    const std::string id = f.id;
    folders_.push_back(std::move(f));
    if (auto saved = SaveLocked(); !saved.ok()) { folders_.pop_back(); return saved.error(); }
    return id;
}

Result<void> ProjectLibrary::Rename(std::string_view id, std::string_view name) {
    const std::string trimmed = Trim(name);
    if (trimmed.empty()) return Invalid("a name cannot be empty");
    if (trimmed.size() > 200) return Invalid("that name is too long");
    std::lock_guard<std::mutex> lock(mutex_);
    if (Project* p = FindProject(id)) { p->name = trimmed; Touch(*p); return SaveLocked(); }
    if (ProjectFolder* f = FindFolder(id)) { f->name = trimmed; return SaveLocked(); }
    return Missing("project or folder", id);
}

Result<std::string> ProjectLibrary::Duplicate(std::string_view projectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    Project* source = FindProject(projectId);
    if (!source) return Missing("project", projectId);
    Project copy = *source;
    copy.id = NextId("project");
    copy.name = source->name + " (copy)";
    copy.created = copy.modified = copy.used = Now();
    copy.archived = false;
    const std::string id = copy.id;
    projects_.push_back(std::move(copy));
    if (auto saved = SaveLocked(); !saved.ok()) { projects_.pop_back(); return saved.error(); }
    return id;
}

Result<void> ProjectLibrary::Move(std::string_view id, std::string_view newParent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!newParent.empty() && !FindFolder(newParent)) return Missing("folder", newParent);
    if (Project* p = FindProject(id)) { p->parent = std::string(newParent); Touch(*p); return SaveLocked(); }
    if (ProjectFolder* f = FindFolder(id)) {
        if (IsInside(newParent, id)) return Invalid("a folder cannot be moved into itself");
        f->parent = std::string(newParent);
        return SaveLocked();
    }
    return Missing("project or folder", id);
}

Result<void> ProjectLibrary::Delete(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto pit = std::find_if(projects_.begin(), projects_.end(), [&](const Project& p) { return p.id == id; });
    if (pit != projects_.end()) { projects_.erase(pit); return SaveLocked(); }
    auto fit = std::find_if(folders_.begin(), folders_.end(), [&](const ProjectFolder& f) { return f.id == id; });
    if (fit == folders_.end()) return Missing("project or folder", id);
    const std::string parent = fit->parent;
    const std::string gone = fit->id;
    folders_.erase(fit);
    for (ProjectFolder& f : folders_) if (f.parent == gone) f.parent = parent;
    for (Project& p : projects_) if (p.parent == gone) p.parent = parent;
    return SaveLocked();
}

#define WITH_PROJECT(id) \
    std::lock_guard<std::mutex> lock(mutex_); \
    Project* project = FindProject(id); \
    if (!project) return Missing("project", id);

Result<void> ProjectLibrary::SetArchived(std::string_view id, bool v) { WITH_PROJECT(id) project->archived = v; Touch(*project); return SaveLocked(); }
Result<void> ProjectLibrary::SetSectionsCollapsed(std::string_view id, bool v) { WITH_PROJECT(id) project->sectionsCollapsed = v; Touch(*project); return SaveLocked(); }
Result<void> ProjectLibrary::SetSectionsLocked(std::string_view id, bool v) { WITH_PROJECT(id) project->sectionsLocked = v; Touch(*project); return SaveLocked(); }
Result<void> ProjectLibrary::SetNotes(std::string_view id, std::string_view notes) { WITH_PROJECT(id) project->notes = std::string(notes); Touch(*project); return SaveLocked(); }

Result<Project> ProjectLibrary::Open(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Project* project = FindProject(id);
    if (!project) return Missing("project", id);
    project->used = Now();
    if (auto saved = SaveLocked(); !saved.ok()) return saved.error();
    return *project;
}

std::vector<Project> ProjectLibrary::RecentlyUsed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Project> recent;
    const int64_t now = Now();
    for (const Project& p : projects_)
        if (!p.archived && p.used > 0 && now - p.used < kFiveDaysMs) recent.push_back(p);
    if (recent.size() < 2) return {};
    std::sort(recent.begin(), recent.end(), [](const Project& a, const Project& b) { return a.used > b.used; });
    return recent;
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

Result<std::vector<std::string>> ProjectLibrary::AddItems(std::string_view projectId, const std::vector<ProjectItem>& items, int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    Project* project = FindProject(projectId);
    if (!project) return Missing("project", projectId);
    if (items.empty()) return std::vector<std::string>{};
    if (project->items.size() + items.size() > kMaxItems) return Invalid(std::format("a project holds at most {} items", kMaxItems));
    for (const ProjectItem& i : items) {
        if (!ItemTypes().contains(i.type)) return Invalid(std::format("'{}' is not something a project can hold", i.type));
        if (i.ref.empty() && i.type != "section") return Invalid(std::format("a {} item needs to say what it is", i.type));
    }

    long long next = 0;
    for (const ProjectItem& i : project->items)
        if (i.id.rfind("pi-", 0) == 0) { try { next = std::max(next, std::stoll(i.id.substr(3))); } catch (...) {} }

    std::vector<ProjectItem> added = items;
    std::vector<std::string> ids;
    for (ProjectItem& i : added) {
        i.id = std::format("pi-{}", ++next);
        if (i.name.empty()) i.name = i.type == "section" ? std::string("Section") : FileNameOf(i.ref);
        ids.push_back(i.id);
    }
    const size_t at = index < 0 || static_cast<size_t>(index) > project->items.size() ? project->items.size() : static_cast<size_t>(index);
    const std::vector<ProjectItem> before = project->items;
    project->items.insert(project->items.begin() + static_cast<long>(at), added.begin(), added.end());
    Touch(*project);
    if (auto saved = SaveLocked(); !saved.ok()) { project->items = before; return saved.error(); }
    return ids;
}

Result<void> ProjectLibrary::MoveItems(std::string_view projectId, const std::vector<int>& indexes, int position) {
    std::lock_guard<std::mutex> lock(mutex_);
    Project* project = FindProject(projectId);
    if (!project) return Missing("project", projectId);
    const int count = static_cast<int>(project->items.size());
    std::set<int> selected;
    for (int i : indexes) {
        if (i < 0 || i >= count) return Invalid(std::format("there is no item {}", i));
        selected.insert(i);
    }
    if (selected.empty()) return {};
    if (position < 0 || position > count) return Invalid("that is not a place in the list");

    // FreeShow's mover: lift the items out, then put them back together at the place, counting from what is left.
    std::vector<ProjectItem> moved, rest;
    int newPos = position;
    for (int i = 0; i < count; ++i) {
        if (selected.contains(i)) { if (i < position) --newPos; moved.push_back(project->items[static_cast<size_t>(i)]); }
        else rest.push_back(project->items[static_cast<size_t>(i)]);
    }
    rest.insert(rest.begin() + newPos, moved.begin(), moved.end());
    project->items = std::move(rest);
    Touch(*project);
    return SaveLocked();
}

#define WITH_ITEM(projectId, index) \
    std::lock_guard<std::mutex> lock(mutex_); \
    Project* project = FindProject(projectId); \
    if (!project) return Missing("project", projectId); \
    if ((index) < 0 || static_cast<size_t>(index) >= project->items.size()) return Invalid(std::format("there is no item {}", index)); \
    ProjectItem& item = project->items[static_cast<size_t>(index)];

Result<void> ProjectLibrary::RemoveItem(std::string_view projectId, int index) {
    WITH_ITEM(projectId, index)
    if (project->sectionsLocked && item.type == "section") return Invalid("this project's sections are locked");
    project->items.erase(project->items.begin() + index);
    Touch(*project);
    return SaveLocked();
}
Result<void> ProjectLibrary::RenameItem(std::string_view projectId, int index, std::string_view name) {
    WITH_ITEM(projectId, index)
    if (Trim(name).empty()) return Invalid("a name cannot be empty");
    if (project->sectionsLocked && item.type == "section") return Invalid("this project's sections are locked");
    item.name = Trim(name).substr(0, 200);
    Touch(*project);
    return SaveLocked();
}
Result<void> ProjectLibrary::SetItemLayout(std::string_view projectId, int index, std::string_view layout) {
    WITH_ITEM(projectId, index)
    if (item.type != "show") return Invalid("only a show has a layout");
    item.layout = std::string(layout);
    Touch(*project);
    return SaveLocked();
}
Result<void> ProjectLibrary::SetItemColor(std::string_view projectId, int index, std::string_view color) {
    WITH_ITEM(projectId, index)
    if (item.type != "section") return Invalid("only a section has a colour");
    item.color = std::string(color);
    Touch(*project);
    return SaveLocked();
}

Result<std::vector<std::string>> ProjectLibrary::DropOnProject(std::string_view projectId, const DropPayload& payload, int index) {
    if (!AcceptsDrop("project", payload.kind))
        return Invalid(std::format("a project does not take '{}'", payload.kind));
    std::vector<ProjectItem> items;
    for (const DropItem& d : payload.items) {
        ProjectItem item;
        item.type = d.type.empty() ? ItemTypeForDrop(payload.kind, d.name.empty() ? d.ref : d.name) : d.type;
        if (item.type.empty()) continue;   // a file that is not something a project holds
        item.ref = d.ref;
        item.name = d.name;
        item.metaJson = d.metaJson.empty() ? "{}" : d.metaJson;
        items.push_back(std::move(item));
    }
    if (items.empty()) return Invalid("nothing that was dropped is something a project can hold");
    return AddItems(projectId, items, index);
}

} // namespace bps::library
