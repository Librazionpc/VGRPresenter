#pragma once

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