#pragma once

// OverlayLibrary: the user's OVERLAYS - graphics that are laid over the slides (a lower third, a
// clock, a recording marker, rounded screen corners) - and the CATEGORIES they are filed under.
// This is the store behind the dock's Overlays tab; the UI only lists and edits what is in here.
//
// Modelled on FreeShow's overlay list (drawer/pages/Overlays + navigation/OverlaysTabs):
//   * a few categories come with the app (marked default: they cannot be renamed or removed);
//   * a few overlays come with the app (marked default: they can be edited or deleted, and a
//     deleted one stays deleted until RestoreDefaults());
//   * an overlay is either filed in a category or "unlabeled".
//
// An overlay is a stack of ELEMENTS laid out on a 1920 x 1080 reference canvas (Box, Text, Clock,
// and two screen-wide treatments: Vignette and rounded Corners). Whatever draws an overlay - the
// tab's preview cards today, the output later - scales those numbers to its own size.
//
// Persistence: one JSON file (categories, overlays, and which defaults were deleted). A missing file
// is an empty library plus the defaults; a damaged one is reported and replaced by the defaults.
//
// Thread-safety: every method may be called from any thread.

#include "core/common/Common.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bps::overlays {

// The canvas every element's numbers are relative to.
inline constexpr double kCanvasWidth = 1920.0;
inline constexpr double kCanvasHeight = 1080.0;

// Filter value for "overlays that are in no category".
inline constexpr std::string_view kUnlabeled = "unlabeled";

struct OverlayElement {
    enum class Kind : int {
        Box = 0,        // a filled / bordered rectangle
        Text = 1,       // text (in its own box, which may be filled too)
        Clock = 2,      // the time - digital text, or an analog face when `analog`
        Vignette = 3,   // the screen edges darkened (or lightened): `background` = colour, `inset` = how far in
        Corners = 4,    // the four screen corners rounded off in `background` colour: `inset` = the radius
    };
    Kind kind = Kind::Box;
    double x = 0, y = 0, width = 0, height = 0;   // on the reference canvas
    std::string background;                        // "#0b57a2", "red", "rgba(0,0,0,0.5)" ... or empty for none
    double radius = 0;                             // corner radius; a value >= half the shorter side makes a pill / circle
    double borderWidth = 0;
    std::string borderColor;
    std::string text;
    std::string textColor = "#ffffff";
    double fontSize = 48;
    bool bold = false;
    bool uppercase = false;
    std::string align = "left";                    // "left" | "center" | "right"
    bool analog = false;                           // Clock: analog face instead of digital digits
    double inset = 0;                              // Vignette / Corners
};

struct Overlay {
    std::string id;
    std::string name;
    std::string color;              // the accent shown on its card; empty for none
    std::string category;           // a category id; empty = unlabeled
    bool isDefault = false;         // came with the app
    bool locked = false;            // stays on screen when the slide changes
    double displayDuration = 0;     // seconds before it leaves by itself; 0 = stays until cleared
    std::vector<OverlayElement> elements;
};

struct OverlayCategory {
    std::string id;
    std::string name;
    std::string icon;               // an icon name the UI knows ("star", "folder", ...)
    bool isDefault = false;         // came with the app: cannot be renamed or removed
};

struct OverlayCounts {
    size_t all = 0;
    size_t unlabeled = 0;
    std::map<std::string, size_t, std::less<>> byCategory;   // category id -> overlays in it
};

class OverlayLibrary {
public:
    // `storageFile`: where the library is kept (its folder is created on save).
    explicit OverlayLibrary(std::string storageFile);

    // Reads the stored library and makes sure the defaults are present (see class comment). A missing file
    // is fine; a damaged one returns ParseError and leaves just the defaults.
    Result<void> Load();

    // ---- categories ----
    std::vector<OverlayCategory> Categories() const;   // defaults first, then the user's, in creation order
    // InvalidArgument for an empty name, AlreadyExists if a category already has that name (any case).
    Result<OverlayCategory> CreateCategory(std::string_view name, std::string_view icon = "folder");
    // NotFound / InvalidState (a default category) / InvalidArgument / AlreadyExists.
    Result<void> RenameCategory(std::string_view id, std::string_view name);
    Result<void> SetCategoryIcon(std::string_view id, std::string_view icon);
    // The overlays that were in it become unlabeled. NotFound / InvalidState (a default category).
    Result<void> DeleteCategory(std::string_view id);

    // ---- overlays ----
    // Every overlay in name order (case-insensitive, numbers in natural order), or only those in `filter` -
    // "" = all, kUnlabeled = in no category, otherwise a category id (an unknown id lists nothing).
    // With `query`, only the overlays whose NAME matches it: every word has to appear, in any order, and a name
    // that starts with the first word ranks above one that only contains it.
    std::vector<Overlay> Overlays(std::string_view filter = {}, std::string_view query = {}) const;
    Result<Overlay> Get(std::string_view id) const;
    OverlayCounts Counts() const;

    // A new overlay with a starter lower-third (a bar and a line of text), in category `categoryId` ("" =
    // unlabeled). `name` empty gives "Overlay", "Overlay 2", ... InvalidArgument for an unknown category.
    Result<Overlay> CreateOverlay(std::string_view name = {}, std::string_view categoryId = {});
    // A copy named "<name> copy", never default and never locked. NotFound for an unknown id.
    Result<Overlay> Duplicate(std::string_view id);
    Result<void> Rename(std::string_view id, std::string_view name);
    Result<void> SetColor(std::string_view id, std::string_view color);
    // `categoryId` "" unlabels it; InvalidArgument for an unknown category.
    Result<void> SetCategory(std::string_view id, std::string_view categoryId);
    Result<void> SetLocked(std::string_view id, bool locked);
    Result<void> SetDisplayDuration(std::string_view id, double seconds);
    // Replaces an overlay's elements (what an editor produces).
    Result<void> SetElements(std::string_view id, std::vector<OverlayElement> elements);
    // Removes it. A default overlay stays gone until RestoreDefaults().
    Result<void> Delete(std::string_view id);

    // Puts back every default overlay that was deleted (existing ones - edited or not - are left alone).
    // Returns how many came back.
    Result<size_t> RestoreDefaults();

    // What the app ships with, for tests and for a "reset" action.
    static std::vector<OverlayCategory> DefaultCategories();
    static std::vector<Overlay> DefaultOverlays();

private:
    Result<void> SaveLocked() const;
    void EnsureDefaultsLocked();
    Overlay* FindLocked(std::string_view id);
    const OverlayCategory* CategoryLocked(std::string_view id) const;
    std::string NextIdLocked(std::string_view prefix) const;

    std::string storageFile_;
    mutable std::mutex mutex_;
    std::vector<OverlayCategory> categories_;
    std::vector<Overlay> overlays_;
    std::set<std::string, std::less<>> deletedDefaults_;   // ids of default overlays the user deleted
};

} // namespace bps::overlays
