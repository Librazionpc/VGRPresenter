#pragma once

// ImportManager (docs/specs/13 §Import Manager): discovers and executes
// registered importers. Knows nothing about specific formats. The pipeline is:
// find importer → validate file → read → extract → normalize → create engine
// assets → generate metadata → thumbnail → index → save.

#include "modules/content/IImporter.hpp"

#include <memory>
#include <mutex>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bps::content {

class ImportManager {
public:
    ImportManager() = default;

    Result<void> Register(std::shared_ptr<IImporter> importer);
    Result<void> Unregister(std::string_view importerName);
    void Clear();
    size_t ImporterCount() const;

    // Extensions covered by all registered importers (sorted, unique).
    std::vector<std::string> SupportedExtensions() const;

    // Find the importer that accepts `extension` (lower-case, with or without
    // leading dot).
    std::shared_ptr<IImporter> FindForExtension(std::string_view extension) const;

    // Run the import pipeline for a file's bytes. Returns produced assets.
    Result<ImportResult> Import(const std::string& fileName,
                                const std::vector<uint8_t>& bytes,
                                const ImportContext& ctx) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IImporter>> importers_;
};

// --- Built-in importers (docs/specs/13): text, json, image passthrough ------
class TextImporter final : public IImporter {
public:
    std::vector<std::string> SupportedExtensions() const override { return {"txt", "md"}; }
    AssetType OutputType() const override { return AssetType::Text; }
    std::string Name() const override { return "TextImporter v1"; }
    bool CanImport(const std::string&, const std::vector<uint8_t>&) const override { return true; }
    Result<ImportResult> Import(const ImportContext& ctx,
                                const std::vector<uint8_t>& bytes) const override;
};

class JsonImporter final : public IImporter {
public:
    std::vector<std::string> SupportedExtensions() const override { return {"json"}; }
    AssetType OutputType() const override { return AssetType::Json; }
    std::string Name() const override { return "JsonImporter v1"; }
    bool CanImport(const std::string&, const std::vector<uint8_t>& head) const override;
    Result<ImportResult> Import(const ImportContext& ctx,
                                const std::vector<uint8_t>& bytes) const override;
};

class ImageImporter final : public IImporter {
public:
    std::vector<std::string> SupportedExtensions() const override {
        return {"png", "jpg", "jpeg", "gif", "svg", "webp", "bmp"};
    }
    AssetType OutputType() const override { return AssetType::Image; }
    std::string Name() const override { return "ImageImporter v1"; }
    bool CanImport(const std::string&, const std::vector<uint8_t>& head) const override;
    Result<ImportResult> Import(const ImportContext& ctx,
                                const std::vector<uint8_t>& bytes) const override;
};

} // namespace bps::content
