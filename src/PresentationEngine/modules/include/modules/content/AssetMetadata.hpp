#pragma once

// Asset metadata record (docs/specs/13 §Asset Database). Metadata is stored
// independently from file contents — the bytes live behind the VFS.

#include "modules/content/AssetTypes.hpp"
#include "modules/content/Uuid.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bps::content {

struct AssetMetadata {
    Uuid uuid;
    std::string name;          // display name
    AssetType type = AssetType::Unknown;
    std::string path;          // VFS path within the library
    std::string vfs;           // mount name ("disk:/", "memory:/", "zip:/...")
    uint64_t sizeBytes = 0;
    std::string hash;          // SHA-256 hex (best-effort; see AssetDatabase)
    std::string checksum;      // crc32 hex, computed on import
    int64_t createdMs = 0;     // epoch ms
    int64_t modifiedMs = 0;
    std::vector<std::string> tags;
    std::string category;
    std::string author;
    std::string description;
    int version = 1;
    bool favorite = false;
    std::vector<std::string> collections;
    std::vector<Uuid> dependencies;
    AssetState state = AssetState::Unknown;

    bool operator==(const AssetMetadata& o) const { return uuid == o.uuid; }
};

} // namespace bps::content
