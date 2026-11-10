#pragma once

#include "platform/IClipboard.hpp"

namespace bps::platform {

// The system clipboard through QClipboard: text, file lists (text/uri-list) and images, on the GUI
// thread, from any caller. ONE implementation for the Linux build, replacing the `wl-paste` / `xclip` /
// `xsel` command lines it used to run (a machine without one of them had no clipboard at all) — Qt
// speaks X11 selections and the Wayland data-device protocol itself.
class QtClipboard final : public IClipboard {
public:
    Result<std::string> ReadText() const override;
    Result<void> WriteText(std::string_view text) override;
    Result<std::vector<std::string>> GetFiles() const override;
    Result<void> SetFiles(const std::vector<std::string>& paths) override;
    Result<ClipboardImage> ReadImage() const override;
    Result<void> WriteImage(const ClipboardImage& image) override;
};

} // namespace bps::platform
