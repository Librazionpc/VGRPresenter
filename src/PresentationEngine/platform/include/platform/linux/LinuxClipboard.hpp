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
    // Bitmaps: the clipboard tools carry image/png, but decoding PNG is the
    // rendering module's job (modules/rendering/PngCodec) and the platform
    // layer sits BELOW that — so image targets report Unsupported here rather
    // than shipping a second, half-written codec. Windows is where the
    // screenshot case comes from.
    Result<ClipboardImage> ReadImage() const override;
    Result<void> WriteImage(const ClipboardImage& image) override;
};

} // namespace bps::platform
