#include "modules/project/PackageManager.hpp"

#include "modules/content/ContentManager.hpp"
#include "modules/project/ProjectManager.hpp"

#include "core/database/DatabaseManager.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <chrono>

namespace bps::project {

namespace {
int64_t MsNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

PackageManager& PackageManager::Instance() {
    static PackageManager instance;
    return instance;
}

Result<void> PackageManager::Export(std::string_view projectId, std::string_view hostDestPath) {
    auto& content = content::ContentManager::Instance();
    auto p = ProjectManager::Instance().Get(projectId);
    if (!p.ok()) return p.error();

    content::ZipWriter writer;
    // 1. Project document.
    auto projectJson = ProjectToJson(p.value());
    auto projectBytes = projectJson.ToString();
    std::vector<uint8_t> projectData(projectBytes.begin(), projectBytes.end());
    if (auto r = writer.AddEntry("project.bpsproj", projectData); !r.ok()) return r.error();

    // 2. Referenced assets (read through CAMS).
    size_t assetCount = 0;
    for (const auto& uuid : p.value().assetUuids) {
        auto meta = content.GetMetadata(content::Uuid::FromString(uuid));
        if (!meta.ok()) continue;   // missing asset → logged below
        auto bytes = content.ReadBytes(meta.value().uuid);
        if (!bytes.ok()) continue;
        if (auto r = writer.AddEntry("assets/" + uuid, bytes.value()); !r.ok()) return r.error();
        ++assetCount;
    }

    // 3. Manifest.
    json::Value::Object manifest;
    manifest["format"] = json::Value::String("bps-project-package");
    manifest["version"] = json::Value::Number(1);
    manifest["projectId"] = json::Value::String(p.value().id);
    manifest["projectName"] = json::Value::String(p.value().name);
    json::Value::Array assets;
    for (const auto& uuid : p.value().assetUuids)
        assets.push_back(json::Value::String(uuid));
    manifest["assetUuids"] = json::Value(std::move(assets));
    manifest["createdAtMs"] = json::Value::Number(static_cast<double>(MsNow()));
    auto manifestText = json::Value(std::move(manifest)).ToString();
    std::vector<uint8_t> manifestData(manifestText.begin(), manifestText.end());
    if (auto r = writer.AddEntry("manifest.json", manifestData); !r.ok()) return r.error();

    auto archive = writer.Finalize();
    if (!archive.ok()) return archive.error();
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    if (auto r = fs.WriteBinary(hostDestPath, archive.value()); !r.ok()) return r.error();

    (void)EventBus::Instance().Publish(
        events::PackageExported{std::string(projectId), std::string(hostDestPath), assetCount});
    Logger::Instance().Info(
        std::format("Package exported ({} assets): {}", assetCount, hostDestPath),
        "PackageManager");
    return Ok();
}

Result<std::string> PackageManager::Import(std::string_view hostPkgPath,
                                           std::string_view targetMount) {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    auto bytes = fs.ReadBinary(hostPkgPath);
    if (!bytes.ok()) return bytes.error();

    // Locate the entries inside the archive (stored method).
    auto& content = content::ContentManager::Instance();
    std::string mountName = targetMount.empty() ? "lib" : std::string(targetMount);
    std::string manifestText;
    std::vector<uint8_t> projectBytes;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> assetEntries;

    // Minimal central-directory scan (mirrors ZipVfs parsing).
    const std::vector<uint8_t>& b = bytes.value();
    if (b.size() < 22) return Error::Make(Err::Project_PackageFailed, "Package", "corrupt archive");
    // Find End Of Central Directory signature.
    size_t eocd = b.size();
    for (size_t i = b.size() - 22 + 1; i-- > 0;) {
        if (b[i] == 0x50 && b[i + 1] == 0x4B && b[i + 2] == 0x05 && b[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == b.size())
        return Error::Make(Err::Project_PackageFailed, "Package", "no central directory");
    uint32_t cdEntries = 0;
    for (int k = 0; k < 2; ++k)
        cdEntries |= static_cast<uint32_t>(b[eocd + 10 + k]) << (8 * k);
    uint32_t cdOffset = 0;
    for (int k = 0; k < 4; ++k)
        cdOffset |= static_cast<uint32_t>(b[eocd + 16 + k]) << (8 * k);

    auto u16 = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8);
    };
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(b[off + k]) << (8 * k);
        return v;
    };

    size_t pos = cdOffset;
    for (uint32_t e = 0; e < cdEntries; ++e) {
        if (pos + 46 > b.size()) break;
        uint16_t method = static_cast<uint16_t>(u16(pos + 10));
        uint32_t compSize = u32(pos + 20);
        uint32_t localOffset = u32(pos + 42);
        uint16_t nameLen = static_cast<uint16_t>(u16(pos + 28));
        uint16_t extraLen = static_cast<uint16_t>(u16(pos + 30));
        uint16_t commentLen = static_cast<uint16_t>(u16(pos + 32));
        if (pos + 46 + nameLen + extraLen + commentLen > b.size()) break;
        std::string name(reinterpret_cast<const char*>(&b[pos + 46]), nameLen);
        if (method != 0) continue;   // stored only (same policy as ZipVfs)
        if (localOffset + 30 > b.size()) continue;   // bounds-check before local reads
        uint16_t lNameLen = static_cast<uint16_t>(u16(localOffset + 26));
        uint16_t lExtraLen = static_cast<uint16_t>(u16(localOffset + 28));
        size_t dataOff = localOffset + 30 + lNameLen + lExtraLen;
        if (dataOff + compSize > b.size()) continue;
        std::vector<uint8_t> data(b.begin() + static_cast<long>(dataOff),
                                  b.begin() + static_cast<long>(dataOff + compSize));
        if (name == "manifest.json") manifestText.assign(data.begin(), data.end());
        else if (name == "project.bpsproj") projectBytes = std::move(data);
        else if (name.starts_with("assets/"))
            assetEntries.emplace_back(name.substr(7), std::move(data));
        pos += 46 + nameLen + extraLen + commentLen;
    }

    if (projectBytes.empty())
        return Error::Make(Err::Project_PackageFailed, "Package", "missing project document");
    auto parsed = json::Parse(std::string(projectBytes.begin(), projectBytes.end()));
    if (!parsed.ok()) return parsed.error();
    auto proj = ProjectFromJson(parsed.value());
    if (!proj.ok()) return proj.error();

    // Import referenced assets through CAMS (CreateText + Save rehydrates the
    // bytes through the VFS; no direct file access). Fresh uuids are assigned
    // and the project document's references are updated to match.
    std::vector<std::string> newUuids;
    for (auto& [uuid, data] : assetEntries) {
        (void)uuid;
        auto meta = content.CreateText("package-asset", content::AssetType::Data, "", {});
        if (!meta.ok()) continue;
        if (auto s = content.Save(meta.value(), data); !s.ok()) continue;
        newUuids.push_back(meta.value().ToString());
    }
    proj.value().assetUuids = newUuids;

    // Register the imported project (fresh id avoids colliding with an already
    // open project of the same name), then attach the rehydrated assets.
    auto created = ProjectManager::Instance().Create(proj.value().name + " (imported)", {});
    if (!created.ok()) return created.error();
    if (auto r = ProjectManager::Instance().SetAssetReferences(created.value().id, newUuids);
        !r.ok())
        return r.error();
    importedCount_.fetch_add(1);
    Logger::Instance().Info(std::format("Package imported: '{}' with {} assets",
                                        created.value().name, newUuids.size()),
                            "PackageManager");
    return Result<std::string>{created.value().id};
}

Result<PackageManifest> PackageManager::Inspect(std::string_view hostPkgPath) {
    auto bytes = platform::PlatformAccessor::Get().Filesystem().ReadBinary(hostPkgPath);
    if (!bytes.ok()) return bytes.error();
    // Find manifest.json entry by scanning the central directory (stored).
    const std::vector<uint8_t>& b = bytes.value();
    size_t eocd = b.size();
    for (size_t i = b.size() - 22 + 1; i-- > 0;) {
        if (b[i] == 0x50 && b[i + 1] == 0x4B && b[i + 2] == 0x05 && b[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == b.size())
        return Error::Make(Err::Project_PackageFailed, "Package", "no central directory");
    uint32_t cdOffset = 0;
    for (int k = 0; k < 4; ++k) cdOffset |= static_cast<uint32_t>(b[eocd + 16 + k]) << (8 * k);
    auto u16 = [&](size_t off) -> uint32_t {
        return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8);
    };
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= static_cast<uint32_t>(b[off + k]) << (8 * k);
        return v;
    };
    size_t pos = cdOffset;
    PackageManifest m;
    while (pos + 46 <= b.size()) {
        if (!(b[pos] == 0x50 && b[pos + 1] == 0x4B)) break;
        uint32_t uncompSize = u32(pos + 24);
        uint32_t localOffset = u32(pos + 42);
        uint16_t nameLen = static_cast<uint16_t>(u16(pos + 28));
        uint16_t extraLen = static_cast<uint16_t>(u16(pos + 30));
        uint16_t commentLen = static_cast<uint16_t>(u16(pos + 32));
        std::string name(reinterpret_cast<const char*>(&b[pos + 46]), nameLen);
        if (name == "manifest.json") {
            if (localOffset + 30 > b.size()) break;   // bounds-check before local reads
            uint16_t lNameLen = static_cast<uint16_t>(u16(localOffset + 26));
            uint16_t lExtraLen = static_cast<uint16_t>(u16(localOffset + 28));
            size_t dataOff = localOffset + 30 + lNameLen + lExtraLen;
            if (dataOff + uncompSize <= b.size()) {
                std::string text(b.begin() + static_cast<long>(dataOff),
                                 b.begin() + static_cast<long>(dataOff + uncompSize));
                auto parsed = json::Parse(text);
                if (!parsed.ok()) return parsed.error();
                const json::Value& v = parsed.value();
                if (const auto* n = v.Find("format")) m.format = std::string(n->asString());
                if (const auto* n = v.Find("version")) m.version = static_cast<int>(n->asInt(1));
                if (const auto* n = v.Find("projectId")) m.projectId = std::string(n->asString());
                if (const auto* n = v.Find("projectName")) m.projectName = std::string(n->asString());
                if (const auto* n = v.Find("createdAtMs"))
                    m.createdAt = std::to_string(n->asInt(0));
                if (const auto* arr = v.Find("assetUuids"))
                    if (const auto* a = arr->asArray())
                        for (const auto& e2 : *a)
                            m.assetUuids.push_back(std::string(e2.asString()));
                return Result<PackageManifest>{m};
            }
        }
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return Error::Make(Err::Project_PackageFailed, "Package", "manifest not found");
}

} // namespace bps::project
