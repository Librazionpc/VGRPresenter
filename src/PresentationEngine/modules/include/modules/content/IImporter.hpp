#pragma once

// IImporter (docs/specs/13 §Import Pipeline): the Open/Closed extension point
// for file formats. The ImportManager knows nothing about PowerPoint, PDF, or
// Bible files — it only discovers and executes registered importers. Adding a
// new format = implement this interface + register. No CAMS code changes.

#include "modules/content/AssetMetadata.hpp"
#include "core/common/Common.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bps::content {

struct ImportContext {
    std::string source;          // host path of the file being imported
    std::string fileName;        // base name with extension
    std::string destinationDir;  // VFS dir to place assets in
    std::string suggestedName;   // optional target name override
    std::vector<std::string> tags;
    std::string category;
    std::string author;
    bool generateThumbnail = true;
    bool compress = true;
    // Progress + cancellation hooks (may be null).
    std::function<void(double fraction, std::string_view stage)> progress;
    std::function<bool()> cancelled;
};

struct ImportedAsset {
    AssetMetadata meta;      // type + name + path are filled by the importer
    std::vector<uint8_t> bytes;
};

struct ImportResult {
    std::vector<ImportedAsset> assets;
    std::vector<std::string> warnings;
    int errorCount = 0;
    std::string importerName;
};

// Pure interface (docs/specs/13 §Every Importer Should Look Like This).
class IImporter {
public:
    virtual ~IImporter() = default;

    // Advertised file extensions this importer accepts, lower-case, e.g. {"txt"}.
    virtual std::vector<std::string> SupportedExtensions() const = 0;

    // Asset type produced by this importer.
    virtual AssetType OutputType() const = 0;

    // Human-readable importer identity ("TextImporter v1", "BibleImporter v2").
    virtual std::string Name() const = 0;

    // Quick capability check beyond the extension (e.g. magic bytes).
    virtual bool CanImport(const std::string& fileName, const std::vector<uint8_t>& head) const = 0;

    // Perform the import. Must fill ImportedAsset.meta.uuid with a fresh UUID,
    // meta.name, meta.type, and meta.path (relative to destinationDir).
    virtual Result<ImportResult> Import(const ImportContext& ctx,
                                        const std::vector<uint8_t>& bytes) const = 0;
};

} // namespace bps::content
