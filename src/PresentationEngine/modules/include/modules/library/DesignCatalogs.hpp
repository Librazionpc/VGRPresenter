#pragma once

#include "modules/library/DesignLibrary.hpp"

#include <memory>
#include <string>

namespace bps::library {
// The shipped configuration of each library (also what tests compare against).
DesignLibraryConfig OverlayLibraryConfig();
DesignLibraryConfig TemplateLibraryConfig();

std::unique_ptr<DesignLibrary> MakeOverlayLibrary(std::string storageFile);
std::unique_ptr<DesignLibrary> MakeTemplateLibrary(std::string storageFile);

std::string SeedStoreJson(const DesignLibraryConfig& config);
} // namespace bps::library