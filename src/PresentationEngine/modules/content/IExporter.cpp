#include "modules/content/IExporter.hpp"

#include "modules/content/Vfs.hpp"
#include "core/config/Json.hpp"
#include "platform/IPlatform.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>

namespace bps::content {

Result<void> ExportManager::Register(std::shared_ptr<IExporter> exporter) {
    if (!exporter)
        return Error::Make(Err::InvalidArgument, "CAMS", "null exporter");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& existing : exporters_) {
        if (existing->Format() == exporter->Format())
            return Error::Make(Err::AlreadyExists, "CAMS",
                               "exporter already registered: " + exporter->Format());
    }
    exporters_.push_back(std::move(exporter));
    return Ok();
}

Result<void> ExportManager::Unregister(std::string_view format) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = exporters_.begin(); it != exporters_.end(); ++it) {
        if ((*it)->Format() == format) {
            exporters_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::NotFound, "CAMS", "exporter not registered: " + std::string(format));
}

void ExportManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    exporters_.clear();
}

size_t ExportManager::ExporterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return exporters_.size();
}

std::vector<std::string> ExportManager::SupportedFormats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& e : exporters_) out.push_back(e->Format());
    std::sort(out.begin(), out.end());
    return out;
}

Result<void> ExportManager::Export(std::string_view format, const ExportContext& ctx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : exporters_) {
        if (e->Format() == format) {
            if (!e->CanExport(ctx.meta.type))
                return Error::Make(Err::Content_UnsupportedFormat, "CAMS",
                                   "exporter " + e->Format() + " cannot export " +
                                       ToString(ctx.meta.type));
            return e->Export(ctx);
        }
    }
    return Error::Make(Err::Content_UnsupportedFormat, "CAMS",
                       "no exporter for format '" + std::string(format) + "'");
}

// ---------------------------------------------------------------------------
// TextExporter
// ---------------------------------------------------------------------------

Result<void> TextExporter::Export(const ExportContext& ctx) const {
    if (ctx.progress) ctx.progress(0.3);
    std::string text(ctx.bytes.begin(), ctx.bytes.end());
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto r = fs.Write(ctx.destinationPath, text);
    if (ctx.progress) ctx.progress(1.0);
    return r;
}

// ---------------------------------------------------------------------------
// JsonExporter
// ---------------------------------------------------------------------------

Result<void> JsonExporter::Export(const ExportContext& ctx) const {
    if (ctx.progress) ctx.progress(0.2);
    json::Value::Object o;
    json::Value::Object metaObj;
    metaObj["uuid"] = json::Value::String(ctx.meta.uuid.ToString());
    metaObj["name"] = json::Value::String(ctx.meta.name);
    metaObj["type"] = json::Value::String(ToString(ctx.meta.type));
    metaObj["path"] = json::Value::String(ctx.meta.path);
    o["metadata"] = json::Value(std::move(metaObj));
    if (ctx.meta.type == AssetType::Text || ctx.meta.type == AssetType::Json ||
        ctx.meta.type == AssetType::Data) {
        o["content"] = json::Value::String(std::string(ctx.bytes.begin(), ctx.bytes.end()));
    } else {
        o["contentBase64"] = json::Value::String("<binary not encoded>");
    }
    if (ctx.progress) ctx.progress(0.6);
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto r = fs.Write(ctx.destinationPath, json::Value(std::move(o)).ToString());
    if (ctx.progress) ctx.progress(1.0);
    return r;
}

// ---------------------------------------------------------------------------
// PackageExporter
// ---------------------------------------------------------------------------

Result<void> PackageExporter::Export(const ExportContext& ctx) const {
    if (ctx.progress) ctx.progress(0.2);
    ZipWriter writer;
    auto r = writer.AddEntry(ctx.meta.path.empty() ? ctx.meta.name : ctx.meta.path, ctx.bytes);
    if (!r.ok()) return r;
    if (ctx.progress) ctx.progress(0.7);
    auto wr = writer.WriteTo(ctx.destinationPath);
    if (ctx.progress) ctx.progress(1.0);
    return wr;
}

} // namespace bps::content
