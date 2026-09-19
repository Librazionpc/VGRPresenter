#pragma once

// IExporter (docs/specs/13 §Export Pipeline): the Open/Closed extension point
// for export formats. The ExportManager knows only how to discover and execute
// registered exporters.

#include "modules/content/AssetMetadata.hpp"
#include "core/common/Common.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::content {

struct ExportContext {
    AssetMetadata meta;                 // the asset being exported
    std::vector<uint8_t> bytes;         // payload bytes (read by caller)
    std::string destinationPath;        // host path to write to
    std::function<void(double)> progress;
    std::function<bool()> cancelled;
};

class IExporter {
public:
    virtual ~IExporter() = default;

    // Format id used in Export(uuid, format), e.g. "text", "json", "package".
    virtual std::string Format() const = 0;
    virtual std::string Name() const = 0;
    virtual bool CanExport(AssetType type) const = 0;

    // Perform the export; writes `destinationPath` (host path).
    virtual Result<void> Export(const ExportContext& ctx) const = 0;
};

class ExportManager {
public:
    ExportManager() = default;

    Result<void> Register(std::shared_ptr<IExporter> exporter);
    Result<void> Unregister(std::string_view format);
    void Clear();
    size_t ExporterCount() const;
    std::vector<std::string> SupportedFormats() const;

    // Export `bytes` of `meta` in `format` to `destinationPath`.
    Result<void> Export(std::string_view format, const ExportContext& ctx) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<IExporter>> exporters_;
};

// --- Built-in exporters ------------------------------------------------------
class TextExporter final : public IExporter {
public:
    std::string Format() const override { return "text"; }
    std::string Name() const override { return "TextExporter v1"; }
    bool CanExport(AssetType type) const override {
        return type == AssetType::Text || type == AssetType::Json || type == AssetType::Data;
    }
    Result<void> Export(const ExportContext& ctx) const override;
};

class JsonExporter final : public IExporter {
public:
    std::string Format() const override { return "json"; }
    std::string Name() const override { return "JsonExporter v1"; }
    bool CanExport(AssetType type) const override { return type != AssetType::Unknown; }
    // Writes {metadata:..., content:<raw bytes as string for text types>}.
    Result<void> Export(const ExportContext& ctx) const override;
};

class PackageExporter final : public IExporter {
public:
    std::string Format() const override { return "package"; }
    std::string Name() const override { return "PackageExporter v1"; }
    bool CanExport(AssetType type) const override { return type == AssetType::Package; }
    Result<void> Export(const ExportContext& ctx) const override;
};

} // namespace bps::content
