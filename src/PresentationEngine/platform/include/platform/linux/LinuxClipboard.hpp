#pragma once

// Linux PAL backend for the clipboard subsystem. Best effort via the system
// clipboard tools (wl-copy/wl-paste on Wayland, xclip/xsel on X11); returns
// Unsupported when no tool is installed.

#include "../IClipboard.hpp"

namespace bps::platform {

class LinuxClipboard final : public IClipboard {
public:
    Result<std::string> ReadText() const override;
    Result<void> WriteText(std::string_view text) override;
    Result<std::vector<std::string>> GetFiles() const override;
    Result<void> SetFiles(const std::vector<std::string>& paths) override;
};

} // namespace bps::platform
