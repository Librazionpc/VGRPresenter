#pragma once

// Projects (FreeShow's Projects panel, in the engine): a project is an ordered list of the things one service or event uses - shows,
// media, audio, overlays, scripture, sections that group them - kept in folders. Opening a project lists its items; clicking one puts it
// on the centre page. The ENGINE owns all of it: the folder tree, what a project may hold, what may be dropped where, how items move,
// which projects were used lately, and the file they are kept in. The UI draws the tree and the items and asks.
//
// A project item points at what it stands for (`ref`: a show's file path, a media file, an overlay id...) - it never copies it.

#include "core/common/Common.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bps::library {

struct ProjectItem {
    std::string id;           // unique within its project
    std::string type;         // show | media | audio | overlay | scripture | section | effect | camera | screen | ndi | player | pdf | image | video | folder
    std::string ref;          // what it points at (a show's file path, a media path, an id); empty for a section
    std::string name;
    std::string layout;       // a show's layout
    std::string color;        // a section's colour
    std::string metaJson = "{}";   // per-kind extras (a player's source, a section's settings...)
};

struct Project {
    std::string id;
    std::string name;
    std::string parent;       // the folder it is in ("" = the top)
    int64_t created = 0;
    int64_t modified = 0;
    int64_t used = 0;         // when it was last opened
    bool archived = false;
    bool sectionsCollapsed = false;
    bool sectionsLocked = false;
    std::string notes;
    std::vector<ProjectItem> items;
};

struct ProjectFolder {
    std::string id;
    std::string name;
    std::string parent;
    int64_t created = 0;
};

// One row of the Projects panel's tree.
struct ProjectTreeRow {
    std::string id;
    std::string type;         // "folder" | "project"
    std::string name;
    std::string parent;
    int depth = 0;
    size_t itemCount = 0;     // a project's items, a folder's children
    bool archived = false;
};

// What is being dragged: one kind of thing, one or more of it.
struct DropItem {
    std::string ref;
    std::string name;
    std::string type;         // overrides the drag's kind for this item ("" = as the drag)
    std::string metaJson = "{}";
};
struct DropPayload {
    std::string kind;         // "show" | "show_drawer" | "media" | "audio" | "audio_effect" | "overlay" | "scripture" | "effect" | "camera" | "screen" | "ndi" | "player" | "files" | "project" | "folder" | "template" | "slide"
    std::vector<DropItem> items;
};

// ---- what may be dropped where (FreeShow's drop.ts) ----
// `area`: "project" | "projects" | "all_slides" | "slides" | "overlays" | "templates" | "edit" | "navigation" | "audio_playlist".
// `reorder` true asks about things already inside the area (moving a show within its project).
bool AcceptsDrop(std::string_view area, std::string_view kind, bool reorder = false);

// The project item type a dragged kind becomes ("show_drawer" -> "show", "audio_effect" -> "audio", "files" -> by extension).
std::string ItemTypeForDrop(std::string_view kind, std::string_view fileName = {});

class ProjectLibrary {
public:
    explicit ProjectLibrary(std::string storageFile);

    Result<void> Load();
    // For tests: what "now" is (milliseconds since the epoch).
    void SetClock(std::function<int64_t()> clock);

    // ---- the tree ----
    std::vector<ProjectTreeRow> Tree() const;                // folders and projects, folders first, by name; archived last
    std::vector<Project> Projects() const;
    std::vector<ProjectFolder> Folders() const;
    Result<Project> Get(std::string_view id) const;
    Result<std::string> Create(std::string_view name, std::string_view parentFolder = {});
    Result<std::string> CreateFolder(std::string_view name, std::string_view parentFolder = {});
    Result<void> Rename(std::string_view id, std::string_view name);         // a project or a folder
    Result<std::string> Duplicate(std::string_view projectId);
    // A project or folder to another folder ("" = the top). A folder cannot go into itself or one of its own folders.
    Result<void> Move(std::string_view id, std::string_view newParent);
    // A project is removed with its items. A folder's projects and folders go up to the folder's parent.
    Result<void> Delete(std::string_view id);
    Result<void> SetArchived(std::string_view projectId, bool archived);
    Result<void> SetSectionsCollapsed(std::string_view projectId, bool collapsed);
    Result<void> SetSectionsLocked(std::string_view projectId, bool locked);
    Result<void> SetNotes(std::string_view projectId, std::string_view notes);

    // Marks a project opened now (the "last used" list). Returns the project.
    Result<Project> Open(std::string_view projectId);
    // Projects opened in the last five days, newest first; nothing unless there are at least two (FreeShow's start-up list).
    std::vector<Project> RecentlyUsed() const;

    // ---- items ----
    // Adds items at `index` (-1 = the end). An unknown type or an empty ref (except a section) is refused, nothing is added.
    Result<std::vector<std::string>> AddItems(std::string_view projectId, const std::vector<ProjectItem>& items, int index = -1);
    // Moves the items at `indexes` so they sit together at `position` (an index in the list as it is now).
    Result<void> MoveItems(std::string_view projectId, const std::vector<int>& indexes, int position);
    Result<void> RemoveItem(std::string_view projectId, int index);
    Result<void> RenameItem(std::string_view projectId, int index, std::string_view name);
    Result<void> SetItemLayout(std::string_view projectId, int index, std::string_view layout);
    Result<void> SetItemColor(std::string_view projectId, int index, std::string_view color);   // sections
    // A drop onto the project: the engine checks the kind against AcceptsDrop, turns it into items and inserts them. Returns the ids added.
    Result<std::vector<std::string>> DropOnProject(std::string_view projectId, const DropPayload& payload, int index = -1);

    const std::string& StorageFile() const noexcept { return storageFile_; }

private:
    Result<void> SaveLocked() const;
    Project* FindProject(std::string_view id);
    ProjectFolder* FindFolder(std::string_view id);
    bool IsInside(std::string_view folder, std::string_view ancestor) const;   // is `folder` `ancestor` or below it?
    std::string NextId(std::string_view prefix) const;
    int64_t Now() const;
    void Touch(Project& p);

    std::string storageFile_;
    mutable std::mutex mutex_;
    std::vector<Project> projects_;
    std::vector<ProjectFolder> folders_;
    std::function<int64_t()> clock_;
};

} // namespace bps::library
