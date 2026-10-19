#pragma once

// The libraries the app keeps, each a DesignLibrary with its own configuration:
//
//   OVERLAYS  - ships FreeShow's "Visuals" category and the overlays filed in it (a recording marker, two
//               clocks, a name lower third, rounded corners, a vignette), all editable and deletable, and
//               restorable with RestoreDefaults().
//   TEMPLATES - ships nothing: the user's own layouts and categories only. A new template starts as a title
//               and a body bound to a slide's fields.
//
// Every block sits on the Edit screen's stage (kStageWidth x kStageHeight), in the shapes the Edit screen
// already draws ("text", "shape", "clock") plus two whole-screen treatments only overlays use:
//   "vignette" - the screen's edges tinted: style.backgroundColor = the tint, meta.inset = how far in it reaches
//   "corners"  - the four screen corners cut round: style.backgroundColor = the colour, meta.inset = the radius

#include "modules/library/DesignLibrary.hpp"

#include <memory>
#include <string>

namespace bps::library {

// The shipped configuration of each library (also what tests compare against).
DesignLibraryConfig OverlayLibraryConfig();
DesignLibraryConfig TemplateLibraryConfig();

std::unique_ptr<DesignLibrary> MakeOverlayLibrary(std::string storageFile);
std::unique_ptr<DesignLibrary> MakeTemplateLibrary(std::string storageFile);

// The store document the installer SHIPS for a library: its catalogs exactly as this build defines
// them, with the shipped-content revision already stamped. Built through the same Load() path a fresh
// install takes, so the seed cannot disagree with what the app itself would create — and generated at
// package time (bps_cli --dump-catalogs), so it can never carry a hand-copied machine's fonts again.
std::string SeedStoreJson(const DesignLibraryConfig& config);

} // namespace bps::library
