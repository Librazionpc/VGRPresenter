#pragma once

// PAL clipboard subsystem (Phase 2): text clipboard access through a common
// interface. On headless Linux this is best-effort via the system clipboard
// tools (xclip / wl-copy); returns Unsupported when none are available.

#include "core/common/Common.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace bps::platform {

class IClipboard {
public:
    virtual ~IClipboard() = default;

    virtual Result<std::string> ReadText() const = 0;
    virtual Result<void> WriteText(std::string_view text) = 0;

    // File lists (DoD §10): absolute paths, not file:// URIs. Best effort —
    // Unsupported when the clipboard provider has no file-list target.
    virtual Result<std::vector<std::string>> GetFiles() const = 0;
    virtual Result<void> SetFiles(const std::vector<std::string>& paths) = 0;
};

} // namespace bps::platform
