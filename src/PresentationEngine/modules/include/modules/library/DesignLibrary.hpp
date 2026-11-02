#pragma once

#include "core/common/Common.hpp"
#include "modules/presentation/PresentationTypes.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bps::library {
// The Edit screen's canvas, in the units every block's x/y/width/height (and font size) are in.
inline constexpr double kStageWidth = 754.0;
inline constexpr double kStageHeight = 428.0;

// Filter value for "designs that are in no category".
inline constexpr std::string_view kUnlabeled = "unlabeled";

inline constexpr int kCatalogRevision = 3;

struct Design {
    std::string id;
    std::string name;
    std::string color;              // the accent shown on its card; empty for none
    std::string category;           // a category id; empty = unlabeled
    std::string contentType;        // templates: the kind of show category it is meant for ("song", "notes"...)
    bool isDefault = false;         // shipped with the app
    bool locked = false;            // overlays: stays on screen when the slide changes
    bool placeUnderSlide = false;   // overlays: drawn beneath the slide's own content
    double displayDuration = 0;     // overlays: seconds before it leaves by itself; 0 = stays until cleared
    std::string background = "transparent";
    std::vector<presentation::ContentBlock> blocks;
    std::string metaJson = "{}";    // frontend extras, opaque here
};

struct DesignCategory {
    std::string id;
    std::string name;
    std::string icon;               // an icon name the UI knows ("star", "folder", ...)
    bool isDefault = false;         // shipped with the app: cannot be renamed, re-iconed or removed
};

struct DesignCounts {
    size_t all = 0;
    size_t unlabeled = 0;
    std::map<std::string, size_t, std::less<>> byCategory;   // category id -> designs in it
};

// What differs between one library and another.
struct DesignLibraryConfig {
    std::string noun = "design";                 // "overlay" - used in messages
    std::string defaultName = "Design";          // a new design is "Design", "Design 2", ...
    std::string idPrefix = "d";                  // design ids are "<prefix>-<n>"
    std::vector<DesignCategory> defaultCategories;
    std::vector<Design> defaultDesigns;
    // Block kinds a design of this library may hold IN ADDITION to the ones every slide has (text, shape, clock ...). The Edit
    // screen offers exactly these in its Add menu, so what a library allows is decided here, not by the UI.
    std::vector<std::string> extraBlockKinds;
    // What a brand-new design holds (empty = a blank canvas).
    std::function<std::vector<presentation::ContentBlock>()> starterBlocks;
};

class DesignLibrary {
public:
    // `storageFile`: where the library is kept (its folder is created on save).
    DesignLibrary(std::string storageFile, DesignLibraryConfig config);

    // Reads the stored library and makes sure what ships is present. A missing file is fine; a damaged one
    // returns ParseError and leaves just what ships.
    Result<void> Load();

    // ---- categories ----
    std::vector<DesignCategory> Categories() const;   // shipped ones first, then the user's, in creation order
    // InvalidArgument for an empty name, AlreadyExists if a category already has that name (any case).
    Result<DesignCategory> CreateCategory(std::string_view name, std::string_view icon = "folder");
    // NotFound / InvalidState (a shipped category) / InvalidArgument / AlreadyExists.
    Result<void> RenameCategory(std::string_view id, std::string_view name);
    Result<void> SetCategoryIcon(std::string_view id, std::string_view icon);
    // The designs that were in it become unlabeled. NotFound / InvalidState (a shipped category).
    Result<void> DeleteCategory(std::string_view id);

    // ---- designs ----
    std::vector<Design> Designs(std::string_view filter = {}, std::string_view query = {}) const;
    Result<Design> Get(std::string_view id) const;
    DesignCounts Counts() const;

    Result<Design> Create(std::string_view name = {}, std::string_view categoryId = {});
    // A copy named "<name> copy", never shipped and never locked. NotFound for an unknown id.
    Result<Design> Duplicate(std::string_view id);
    Result<void> Rename(std::string_view id, std::string_view name);
    Result<void> SetColor(std::string_view id, std::string_view color);
    // `categoryId` "" unlabels it; InvalidArgument for an unknown category.
    Result<void> SetCategory(std::string_view id, std::string_view categoryId);
    Result<void> SetContentType(std::string_view id, std::string_view contentType);
    Result<void> SetLocked(std::string_view id, bool locked);
    Result<void> SetPlaceUnderSlide(std::string_view id, bool under);
    Result<void> SetDisplayDuration(std::string_view id, double seconds);
    // Removes it. A shipped design stays gone until RestoreDefaults().
    Result<void> Delete(std::string_view id);

    // Puts back every shipped design that was deleted (existing ones - edited or not - are left alone).
    // Returns how many came back.
    Result<size_t> RestoreDefaults();

    // ---- content: what the Edit screen edits ----
    // Replaces the canvas: its background and every block (what the Edit screen sends when an edit settles).
    Result<void> SetContent(std::string_view id, std::string background, std::vector<presentation::ContentBlock> blocks);
    Result<std::string> AddBlock(std::string_view id, presentation::ContentBlock block, std::string_view above = {});
    // A copy offset by (20, 20), directly above the original; returns its id.
    Result<std::string> DuplicateBlock(std::string_view id, std::string_view blockId);
    Result<void> RemoveBlock(std::string_view id, std::string_view blockId);
    Result<presentation::ContentBlock> Block(std::string_view id, std::string_view blockId) const;

    // The design as a slide template (its blocks keep their `bind`), e.g. to embed a library template in a show.
    Result<presentation::SlideTemplate> AsTemplate(std::string_view id) const;

    std::string StoreJson() const;

    const DesignLibraryConfig& Config() const { return config_; }

private:
    std::string BuildStoreJsonLocked() const;
    // Writes the text BuildStoreJsonLocked() produced. PAL: NEVER call with
    // mutex_ held. "" / no store path is a no-op.
    Result<void> CommitSave(const std::string& json) const;
    void EnsureDefaultsLocked();
    Design* FindLocked(std::string_view id);
    const DesignCategory* CategoryLocked(std::string_view id) const;
    std::string NextIdLocked(std::string_view prefix) const;
    Error Missing() const;
    // One edit of one design: applies `change`, saves, and puts the design back as it was if the save fails.
    template <typename Change>
    Result<void> Edit(std::string_view id, Change&& change);

    std::string storageFile_;
    DesignLibraryConfig config_;
    mutable std::mutex mutex_;
    std::vector<DesignCategory> categories_;
    std::vector<Design> designs_;
    std::set<std::string, std::less<>> deletedDefaults_;   // ids of shipped designs the user deleted
};
} // namespace bps::library