#pragma once

// WorkspaceManager (docs/specs/15 §Workspace Manager): user workspace state —
// open documents, selected displays, current presentation/theme, zoom, recently
// used items. Fully serializable (JSON via the DatabaseManager) so the frontend
// can restore window layout on startup. The engine stores *state*, never
// windowing details.

#include "core/config/Json.hpp"
#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

class WorkspaceManager {
public:
    static WorkspaceManager& Instance();

    // --- State ---
    void SetOpenDocuments(const std::vector<std::string>& docIds);
    std::vector<std::string> OpenDocuments() const;
    void AddOpenDocument(std::string_view docId);
    void RemoveOpenDocument(std::string_view docId);

    void SetSelectedDisplays(const std::vector<std::string>& displayIds);
    std::vector<std::string> SelectedDisplays() const;

    void SetCurrentPresentation(std::string_view assetUuid);
    std::string CurrentPresentation() const;

    void SetCurrentTheme(std::string_view themeId);
    std::string CurrentTheme() const;

    void SetZoom(double zoom);
    double Zoom() const;

    void AddRecentlyUsed(std::string_view kind, std::string_view id, std::string_view name);
    std::vector<std::string> RecentlyUsed(std::string_view kind, int limit = 10) const;

    // --- Persistence (collection "workspace") ---
    Result<void> Save();
    Result<void> Load();
    json::Value ToJson() const;
    Result<void> FromJson(const json::Value& v);

private:
    WorkspaceManager() = default;

    mutable std::mutex mutex_;
    std::vector<std::string> openDocuments_;
    std::vector<std::string> selectedDisplays_;
    std::string currentPresentation_;
    std::string currentTheme_;
    double zoom_ = 1.0;
    struct Recent {
        std::string kind;
        std::string id;
        std::string name;
        int64_t ts = 0;
    };
    std::vector<Recent> recent_;
    int recentLimit_ = 25;
};

} // namespace bps::project
