#include "modules/content/ImportManager.hpp"

#include "core/config/Json.hpp"

#include <algorithm>
#include <cstring>

namespace bps::content {

namespace {

std::string LowerExt(std::string_view extension) {
    std::string ext;
    for (char c : extension)
        ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    return ext;
}

bool HasMagic(const std::vector<uint8_t>& head, const char* magic, size_t len) {
    return head.size() >= len && std::memcmp(head.data(), magic, len) == 0;
}

} // namespace

Result<void> ImportManager::Register(std::shared_ptr<IImporter> importer) {
    if (!importer)
        return Error::Make(Err::InvalidArgument, "CAMS", "null importer");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& existing : importers_) {
        if (existing->Name() == importer->Name())
            return Error::Make(Err::AlreadyExists, "CAMS", "importer already registered: " +
                                                               importer->Name());
    }
    importers_.push_back(std::move(importer));
    return Ok();
}

Result<void> ImportManager::Unregister(std::string_view importerName) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = importers_.begin(); it != importers_.end(); ++it) {
        if ((*it)->Name() == importerName) {
            importers_.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::NotFound, "CAMS", "importer not registered: " +
                                                  std::string(importerName));
}

void ImportManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    importers_.clear();
}

size_t ImportManager::ImporterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return importers_.size();
}

std::vector<std::string> ImportManager::SupportedExtensions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& imp : importers_)
        for (const auto& ext : imp->SupportedExtensions()) out.push_back(ext);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::shared_ptr<IImporter> ImportManager::FindForExtension(std::string_view extension) const {
    std::string ext = LowerExt(extension);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& imp : importers_) {
        for (const auto& supported : imp->SupportedExtensions()) {
            if (LowerExt(supported) == ext) return imp;
        }
    }
    return nullptr;
}

Result<ImportResult> ImportManager::Import(const std::string& fileName,
                                           const std::vector<uint8_t>& bytes,
                                           const ImportContext& ctx) const {
    auto dot = fileName.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : fileName.substr(dot + 1);
    auto importer = FindForExtension(ext);
    if (!importer)
        return Error::Make(Err::Content_UnsupportedFormat, "CAMS",
                           "no importer for ." + ext);
    const size_t headLen = std::min<size_t>(16, bytes.size());
    std::vector<uint8_t> head(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(headLen));
    if (!importer->CanImport(fileName, head))
        return Error::Make(Err::Content_ValidationFailed, "CAMS",
                           "importer " + importer->Name() + " rejected " + fileName);
    if (ctx.progress) ctx.progress(0.1, "importer selected");
    auto result = importer->Import(ctx, bytes);
    if (!result.ok()) return result;
    if (ctx.progress) ctx.progress(1.0, "import complete");
    return result;
}

// ---------------------------------------------------------------------------
// TextImporter
// ---------------------------------------------------------------------------

Result<ImportResult> TextImporter::Import(const ImportContext& ctx,
                                          const std::vector<uint8_t>& bytes) const {
    ImportResult result;
    result.importerName = Name();
    ImportedAsset a;
    a.meta.uuid = Uuid::Generate();
    a.meta.type = AssetType::Text;
    a.meta.name = ctx.suggestedName.empty()
                      ? (ctx.fileName.empty() ? "text" : ctx.fileName)
                      : ctx.suggestedName;
    a.meta.path = ctx.destinationDir.empty()
                      ? a.meta.name
                      : ctx.destinationDir + "/" + a.meta.name;
    a.meta.tags = ctx.tags;
    a.meta.category = ctx.category;
    a.meta.author = ctx.author;
    a.bytes = bytes;
    result.assets.push_back(std::move(a));
    return Result<ImportResult>{std::move(result)};
}

// ---------------------------------------------------------------------------
// JsonImporter
// ---------------------------------------------------------------------------

bool JsonImporter::CanImport(const std::string&, const std::vector<uint8_t>& head) const {
    // Accept if it starts with { or [ after optional whitespace.
    for (uint8_t b : head) {
        if (b == ' ' || b == '\t' || b == '\r' || b == '\n') continue;
        return b == '{' || b == '[';
    }
    return false;
}

Result<ImportResult> JsonImporter::Import(const ImportContext& ctx,
                                          const std::vector<uint8_t>& bytes) const {
    std::string text(bytes.begin(), bytes.end());
    auto parsed = json::Parse(text);
    if (!parsed.ok())
        return Error::Make(Err::Content_ValidationFailed, "CAMS",
                           "invalid JSON: " + parsed.error().message);
    ImportResult result;
    result.importerName = Name();
    ImportedAsset a;
    a.meta.uuid = Uuid::Generate();
    a.meta.type = AssetType::Json;
    a.meta.name = ctx.suggestedName.empty()
                      ? (ctx.fileName.empty() ? "data" : ctx.fileName)
                      : ctx.suggestedName;
    a.meta.path = ctx.destinationDir.empty()
                      ? a.meta.name
                      : ctx.destinationDir + "/" + a.meta.name;
    a.meta.tags = ctx.tags;
    a.meta.category = ctx.category;
    a.bytes = bytes;
    result.assets.push_back(std::move(a));
    return Result<ImportResult>{std::move(result)};
}

// ---------------------------------------------------------------------------
// ImageImporter
// ---------------------------------------------------------------------------

bool ImageImporter::CanImport(const std::string& fileName, const std::vector<uint8_t>& head) const {
    auto lower = LowerExt([&] {
        auto dot = fileName.find_last_of('.');
        return dot == std::string::npos ? std::string{} : fileName.substr(dot + 1);
    }());
    if (lower == "png") return HasMagic(head, "\x89PNG", 4);
    if (lower == "jpg" || lower == "jpeg") return head.size() >= 3 && head[0] == 0xFF && head[1] == 0xD8;
    if (lower == "gif") return HasMagic(head, "GIF8", 4);
    if (lower == "svg") return head.size() >= 4 && std::memcmp(head.data(), "<svg", 4) == 0;
    if (lower == "webp") return HasMagic(head, "RIFF", 4);
    return true;   // unknown but claimed extension: pass through
}

Result<ImportResult> ImageImporter::Import(const ImportContext& ctx,
                                           const std::vector<uint8_t>& bytes) const {
    ImportResult result;
    result.importerName = Name();
    ImportedAsset a;
    a.meta.uuid = Uuid::Generate();
    a.meta.type = AssetType::Image;
    a.meta.name = ctx.suggestedName.empty()
                      ? (ctx.fileName.empty() ? "image" : ctx.fileName)
                      : ctx.suggestedName;
    a.meta.path = ctx.destinationDir.empty()
                      ? a.meta.name
                      : ctx.destinationDir + "/" + a.meta.name;
    a.meta.tags = ctx.tags;
    a.meta.category = ctx.category;
    a.bytes = bytes;
    result.assets.push_back(std::move(a));
    return Result<ImportResult>{std::move(result)};
}

} // namespace bps::content
