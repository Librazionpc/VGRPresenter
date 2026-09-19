#pragma once

// Native .vgr format (docs/specs/22). The authoritative representation of
// everything created in the platform. One extension, many internal document
// types (Show, Presentation, Template, Theme, Workspace, Playlist, Package,
// Asset Collection, Config Profile). Sectioned container with a JSON header,
// CRC32 integrity per section, dependency manifest, embedded assets, and
// version detection + migration hooks.

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bps::vgr {

inline constexpr char kMagic[9] = "BPSVGR01";        // 8 bytes + NUL
inline constexpr uint32_t kFormatVersion = 1;

// --- Internal document types (docs/specs/22 §Internal Document Types) ----------
enum class DocumentType : int {
    Show = 0,
    Presentation,
    Template,
    Theme,
    Workspace,
    Playlist,
    ProjectPackage,
    AssetCollection,
    ConfigProfile,
    Unknown,
};

inline const char* ToString(DocumentType t) {
    switch (t) {
        case DocumentType::Show:             return "Show";
        case DocumentType::Presentation:     return "Presentation";
        case DocumentType::Template:         return "Template";
        case DocumentType::Theme:            return "Theme";
        case DocumentType::Workspace:        return "Workspace";
        case DocumentType::Playlist:         return "Playlist";
        case DocumentType::ProjectPackage:   return "ProjectPackage";
        case DocumentType::AssetCollection:  return "AssetCollection";
        case DocumentType::ConfigProfile:    return "ConfigProfile";
        case DocumentType::Unknown:          return "Unknown";
    }
    return "Unknown";
}

// --- Header metadata -------------------------------------------------------------
struct VgrHeader {
    uint32_t formatVersion = kFormatVersion;
    std::string engineVersion;      // e.g. "1.0.0"
    DocumentType type = DocumentType::Unknown;
    std::string uuid;
    std::string name;
    int64_t createdMs = 0;
    int64_t modifiedMs = 0;
    std::string author;
    std::string compression = "stored";     // "stored" | "rle"
    std::string integrityHash;              // whole-file hash (hex)
    std::vector<std::string> dependencyIds; // dependency manifest
    std::vector<std::string> externalRefs;  // external asset references
    std::map<std::string, std::string, std::less<>> searchMetadata;
    std::map<std::string, std::string, std::less<>> customMetadata;
};

// --- One section of the container -------------------------------------------------
struct VgrSection {
    std::string name;                       // "document", "assets", "theme"...
    std::string kind;                       // "json" | "binary" | "asset"
    std::vector<uint8_t> payload;           // raw payload (compressed per header)
    uint32_t crc32 = 0;
};

// --- A full .vgr document ----------------------------------------------------------
struct VgrDocument {
    VgrHeader header;
    // Primary content: the JSON document body.
    std::string documentJson;
    // Sections: named payloads (embedded assets, previews, binaries).
    std::vector<VgrSection> sections;
    // Embedded asset registry (name -> bytes) — convenient for asset bundling.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> embeddedAssets;
};

// --- Serializer ---------------------------------------------------------------------
class VgrSerializer {
public:
    // Serializes a document to the binary container. Compression is applied
    // per section (stored today; RLE for asset payloads).
    static Result<std::vector<uint8_t>> Write(const VgrDocument& doc);

    // Deserializes + validates (magic, version, CRC). Migration hooks run via
    // Migrate() before the document is returned.
    static Result<VgrDocument> Read(const std::vector<uint8_t>& bytes);

    // Version detection: returns the format version from the bytes (0 = bad).
    static uint32_t PeekVersion(const std::vector<uint8_t>& bytes);

    // Migration hook: upgrades a document from an older format version.
    static Result<void> Migrate(VgrDocument& doc, uint32_t fromVersion);
};

} // namespace bps::vgr
