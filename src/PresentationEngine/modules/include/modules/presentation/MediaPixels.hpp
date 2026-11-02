#pragma once

// ---- a placed media file: decoded, cached, and painted ----------------------

#include <string>
#include <string_view>

#include "modules/presentation/PresentationTypes.hpp"   // ContentBlock
#include "modules/rendering/Effects.hpp"                // SlideFilterValues
#include "modules/rendering/IGraphicsBackend.hpp"       // RgbaImage
#include "modules/rendering/RenderTypes.hpp"            // Rect, Size

namespace bps::presentation {
// ---- A placed media file's OWN values (the Edit screen's MEDIA tab) ---------
struct MediaItemValues {
    std::string fit;   // "" (contain) | contain | cover | fill | blur
    bool flipH = false;
    bool flipV = false;
    // Percentages of the source picture, 0–90.
    double cropTop = 0, cropRight = 0, cropBottom = 0, cropLeft = 0;
    rendering::SlideFilterValues filter;
    double sepia = 0.0;    // 0–1 (not part of the slide filter set)
    double opacity = 1.0;  // 0–1, the file's own transparency (a filter row)
    // FreeShow's own blend name ("normal", "screen", "soft-light" …) — how the
    // picture composites onto what is already under it. Empty = normal.
    std::string blend;

    bool HasCrop() const {
        return cropTop > 0 || cropRight > 0 || cropBottom > 0 || cropLeft > 0;
    }
    bool HasFilter() const { return !filter.IsIdentity() || sepia > 0.0 || opacity < 1.0; }
};

// ---- the style background image, read+decoded once per path ----------------
rendering::RgbaImage LoadImageCached(const std::string& path);
// ---- a placed video item's poster frame (Media Foundation's own still) -----
rendering::RgbaImage LoadVideoPosterCached(const std::string& path);

// ---- geometry --------------------------------------------------------------
// Contain-fit INSIDE a rect: the whole picture inside the item's own box,
// centred, never cropped.
rendering::Rect ContainRectIn(int imgW, int imgH, const rendering::Rect& box);
// Contain-fit onto the stage (what a media file on air gets — the QML tile draws
// the same file with PreserveAspectFit, so both show ONE picture).
rendering::Rect ContainRect(int imgW, int imgH, const rendering::Size& stage);
// Cover-fit onto the stage (a style background's default).
rendering::Rect CoverRect(int imgW, int imgH, const rendering::Size& stage);

// ---- the picture a placed item draws ---------------------------------------
// The file a placed media item carries (its meta `src`). Empty = nothing to
// paint (an item whose picker never completed).
std::string MediaSrcOf(const ContentBlock& block);
// The item's own values, read from its `media` meta object.
MediaItemValues ParseMediaItemValues(std::string_view metaJson);
// Crop, then flips, then the filter set in CSS order, then the file's opacity.
rendering::RgbaImage ApplyMediaPixelWork(rendering::RgbaImage picture,
                                         const MediaItemValues& v);
rendering::RgbaImage CropToAspect(const rendering::RgbaImage& src, double boxW,
                                  double boxH);
} // namespace bps::presentation