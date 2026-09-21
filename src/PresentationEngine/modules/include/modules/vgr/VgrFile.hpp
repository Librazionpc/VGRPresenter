#pragma once

// VgrFile (docs/specs/22): reading and writing .vgr FILES — the disk side of
// the container that VgrSerializer builds in memory. Every module that persists
// a .vgr document (shows, templates, themes...) goes through here so they all get
// the same guarantees instead of each re-implementing file safety:
//
//   Read  — the whole file is validated (magic, version, every section's CRC32)
//           before anything is returned; a damaged/truncated file is an error.
//   Write — crash-safe: the new bytes are written beside the target, read back and
//           verified, and only then swapped in. The previous file is parked as
//           `.bak` during the swap and restored if it fails, so at every instant
//           there is one complete file on disk — never a half-written one.

#include "core/common/Common.hpp"
#include "modules/vgr/VgrFormat.hpp"

#include <string>

namespace bps::vgr {

class VgrFile {
public:
    static Result<VgrDocument> Read(const std::string& path);
    // Serializes `doc` and writes it to `path` (verified + atomic, see above).
    static Result<void> Write(const std::string& path, const VgrDocument& doc);
};

} // namespace bps::vgr
