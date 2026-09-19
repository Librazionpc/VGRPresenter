#include "modules/project/ProjectManager.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <chrono>
#include <cstring>
#include <format>

namespace bps::project {

namespace {

int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string GenerateId() {
    // v4-style UUID without OS-specific calls (PAL DoD §1).
    unsigned char b[16] = {0};
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t v = static_cast<uint64_t>(t);
    v ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&v)) << 3;
    v ^= std::hash<std::thread::id>{}(std::this_thread::get_id());
    for (int i = 0; i < 16; ++i)
        b[i] = static_cast<unsigned char>((v >> ((i % 8) * 8)) & 0xFF);
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);
    return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
                       "{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                       b[10], b[11], b[12], b[13], b[14], b[15]);
}

std::string SanitizeName(std::string_view name) {
    std::string out;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    return out.empty() ? "Untitled" : out;
}

} // namespace

ProjectManager& ProjectManager::Instance() {
    static ProjectManager instance;
    return instance;
}

void ProjectManager::SetDataDir(std::string_view dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    dataDir_ = std::string(dir);
}

Result<Project> ProjectManager::Create(std::string_view name, std::string_view templateId) {
    Project p;
    p.id = GenerateId();
    p.name = std::string(name);
    if (p.name.empty()) p.name = "Untitled Project";
    p.createdAtMs = MsNow();
    p.modifiedAtMs = p.createdAtMs;
    p.settings.defaultTemplateId = std::string(templateId);

    if (auto r = registry_.Register(p); !r.ok()) return r.error();
    if (auto r = Persist(p); !r.ok()) {
        (void)registry_.Unregister(p.id);
        return r.error();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeId_.empty()) activeId_ = p.id;
    }
    (void)EventBus::Instance().Publish(events::ProjectCreated{p.id, p.name});
    Logger::Instance().Info("Project created: '" + p.name + "'", "ProjectManager");
    return Result<Project>{p};
}

Result<Project> ProjectManager::Open(std::string_view hostPath) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto text = fs.ReadText(hostPath);
    if (!text.ok()) return text.error();
    auto parsed = json::Parse(text.value());
    if (!parsed.ok()) return parsed.error();
    auto p = ProjectFromJson(parsed.value());
    if (!p.ok()) return p.error();
    // The on-disk path is authoritative.
    p.value().path = std::string(hostPath);
    p.value().modifiedAtMs = MsNow();

    if (auto r = registry_.Register(p.value()); !r.ok())
        return Error::Make(Err::Project_AlreadyOpen, "ProjectManager",
                           "project '" + p.value().name + "' is already open");
    if (auto r = Persist(p.value()); !r.ok()) {
        (void)registry_.Unregister(p.value().id);
        return r.error();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeId_ = p.value().id;
    }
    (void)EventBus::Instance().Publish(events::ProjectOpened{p.value().id, p.value().name});
    Logger::Instance().Info("Project opened: '" + p.value().name + "'", "ProjectManager");
    return p;
}

Result<void> ProjectManager::Persist(const Project& p) {
    auto& db = DatabaseManager::Instance();
    return db.Put("projects", p.id, ProjectToJson(p));
}

Result<void> ProjectManager::Save(std::string_view id, bool autosave) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    Project proj = p.value();
    proj.modifiedAtMs = MsNow();

    // Write the project file through the PAL (create dirs as needed).
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::string path = proj.path;
    if (path.empty()) {
        path = dataDir_ + "/" + SanitizeName(proj.name) + ".bpsproj";
        if (!dataDir_.empty()) (void)fs.CreateDirectories(dataDir_);
        proj.path = path;
    }
    auto bytes = ProjectToJson(proj).ToString();
    if (auto r = fs.Write(path, bytes); !r.ok()) return r.error();

    if (auto r = registry_.Update(proj); !r.ok()) return r.error();
    if (auto r = Persist(proj); !r.ok()) return r.error();
    (void)EventBus::Instance().Publish(events::ProjectSaved{proj.id, proj.name, autosave});
    return Ok();
}

Result<Project> ProjectManager::SaveAs(std::string_view id, std::string_view hostPath) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    Project proj = p.value();
    proj.path = std::string(hostPath);
    proj.modifiedAtMs = MsNow();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto r = fs.Write(proj.path, ProjectToJson(proj).ToString()); !r.ok()) return r.error();
    if (auto r = registry_.Update(proj); !r.ok()) return r.error();
    if (auto r = Persist(proj); !r.ok()) return r.error();
    (void)EventBus::Instance().Publish(events::ProjectSaved{proj.id, proj.name, false});
    return Result<Project>{proj};
}

Result<void> ProjectManager::Close(std::string_view id) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    if (auto r = registry_.Unregister(id); !r.ok()) return r.error();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeId_ == id) activeId_.clear();
    }
    (void)EventBus::Instance().Publish(events::ProjectClosed{p.value().id, p.value().name});
    Logger::Instance().Info("Project closed: '" + p.value().name + "'", "ProjectManager");
    return Ok();
}

Result<Project> ProjectManager::Rename(std::string_view id, std::string_view newName) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    if (std::string(newName).empty())
        return Error::Make(Err::InvalidArgument, "ProjectManager", "name must not be empty");
    Project proj = p.value();
    proj.name = std::string(newName);
    proj.modifiedAtMs = MsNow();
    if (auto r = registry_.Update(proj); !r.ok()) return r.error();
    if (auto r = Persist(proj); !r.ok()) return r.error();
    // Keep the on-disk file in sync so Open() sees the new name.
    if (!proj.path.empty()) {
        if (auto w = platform::PlatformAccessor::Get().Filesystem().Write(
                proj.path, ProjectToJson(proj).ToString());
            !w.ok())
            return w.error();
    }
    return Result<Project>{proj};
}

Result<Project> ProjectManager::Duplicate(std::string_view id) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    Project copy = p.value();
    copy.id = GenerateId();
    copy.name = copy.name + " (copy)";
    copy.path.clear();   // unsaved until SaveAs
    copy.createdAtMs = MsNow();
    copy.modifiedAtMs = copy.createdAtMs;
    if (auto r = registry_.Register(copy); !r.ok()) return r.error();
    if (auto r = Persist(copy); !r.ok()) {
        (void)registry_.Unregister(copy.id);
        return r.error();
    }
    return Result<Project>{copy};
}

Result<void> ProjectManager::SetAssetReferences(std::string_view id,
                                                const std::vector<std::string>& uuids) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    Project proj = p.value();
    proj.assetUuids = uuids;
    proj.modifiedAtMs = MsNow();
    if (auto r = registry_.Update(proj); !r.ok()) return r.error();
    if (auto r = Persist(proj); !r.ok()) return r.error();
    if (!proj.path.empty()) {
        if (auto w = platform::PlatformAccessor::Get().Filesystem().Write(
                proj.path, ProjectToJson(proj).ToString());
            !w.ok())
            return w.error();
    }
    (void)EventBus::Instance().Publish(events::ProjectSaved{proj.id, proj.name, false});
    return Ok();
}

Result<void> ProjectManager::Delete(std::string_view id) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    // Remove the persisted document + the project file (best effort).
    (void)DatabaseManager::Instance().Remove("projects", std::string(id));
    if (!p.value().path.empty())
        (void)platform::PlatformAccessor::Get().Filesystem().Remove(p.value().path);
    if (auto r = registry_.Unregister(id); !r.ok()) return r.error();
    Logger::Instance().Info("Project deleted: '" + p.value().name + "'", "ProjectManager");
    return Ok();
}

Result<void> ProjectManager::Archive(std::string_view id, bool archived) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    Project proj = p.value();
    proj.archived = archived;
    proj.modifiedAtMs = MsNow();
    if (auto r = registry_.Update(proj); !r.ok()) return r.error();
    return Persist(proj);
}

std::vector<Project> ProjectManager::OpenProjects() const { return registry_.All(); }

Result<Project> ProjectManager::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeId_.empty())
        return Error::Make(Err::Project_NotFound, "ProjectManager", "no active project");
    return registry_.Find(activeId_);
}

Result<void> ProjectManager::SetActive(std::string_view id) {
    auto p = registry_.Find(id);
    if (!p.ok()) return p.error();
    std::lock_guard<std::mutex> lock(mutex_);
    activeId_ = std::string(id);
    return Ok();
}

Result<Project> ProjectManager::Get(std::string_view id) const { return registry_.Find(id); }

Result<void> ProjectManager::LoadAll() {
    auto& db = DatabaseManager::Instance();
    for (const auto& key : db.Keys("projects")) {
        auto doc = db.Get("projects", key);
        if (!doc.ok()) continue;
        auto p = ProjectFromJson(doc.value());
        if (!p.ok()) continue;
        // Only restore non-archived projects that were open at shutdown.
        if (p.value().archived) continue;
        (void)registry_.Register(p.value());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeId_.empty()) activeId_ = p.value().id;
        }
        Logger::Instance().Info("Restored project: '" + p.value().name + "'", "ProjectManager");
    }
    return Ok();
}

Result<void> ProjectManager::AutosaveAll() {
    for (const auto& p : registry_.All()) {
        if (!p.archived) (void)Save(p.id, true);
    }
    return Ok();
}

} // namespace bps::project
