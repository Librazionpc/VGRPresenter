#pragma once

// Reading what import files are wrapped in: zip archives (Quelea song packs, PowerPoint and Word files are zips of XML), and text in
// whatever encoding the program that wrote it used.

#include "core/common/Common.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::import {

struct ArchiveEntry {
    std::string name;   // the path inside the archive: "ppt/slides/slide1.xml"
    std::string data;   // the file's bytes, decompressed
};

// True when the bytes begin like a zip file.
bool LooksLikeZip(std::string_view bytes);

// Every file in a zip archive (folders are left out). Stored and deflated entries are read; a file bigger than `maxEntryBytes`
// once decompressed is refused (a zip bomb must not eat the machine).
Result<std::vector<ArchiveEntry>> ReadZip(std::string_view bytes, size_t maxEntryBytes = 64u * 1024u * 1024u);

// The text as UTF-8: a UTF-8 or UTF-16 byte order mark is honoured, valid UTF-8 is kept, anything else is read as Windows-1252
// (what the Windows song programs write).
std::string ToUtf8(std::string_view bytes);

// Decodes base64 (whitespace and padding are tolerated; anything else that is not base64 is skipped).
std::string Base64Decode(std::string_view text);

} // namespace bps::import
