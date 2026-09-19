#pragma once

// FavoritesManager (docs/specs/15 §Favorites Manager): lets users mark
// projects, assets, templates, songs and bibles as favorites. Persisted via
// the DatabaseManager.

#include "interfaces/IService.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace bps::project {

struct FavoriteItem {
    std::string kind;   // "project" | "asset" | "template" | "song" | "bible"
    std::string id;
    std::string name;
    int64_t addedAtMs = 0;
};

class FavoritesManager {
public:
    static FavoritesManager& Instance();

    Result<void> Add(std::string_view kind, std::string_view id, std::string_view name);
    Result<void> Remove(std::string_view kind, std::string_view id);
    bool IsFavorite(std::string_view kind, std::string_view id) const;
    std::vector<FavoriteItem> List(std::string_view kind = {}) const;

    Result<void> Save();
    Result<void> Load();
    size_t Count() const;

private:
    FavoritesManager() = default;

    mutable std::mutex mutex_;
    std::vector<FavoriteItem> items_;
};

} // namespace bps::project
