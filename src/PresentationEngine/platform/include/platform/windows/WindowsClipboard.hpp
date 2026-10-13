#pragma once

// Windows PAL backend for the clipboard subsystem: text (CF_UNICODETEXT),
// file lists (CF_HDROP) and bitmaps (CF_DIB / CF_BITMAP).

#include "platform/IClipboard.hpp"

namespace bps::platform {

class WindowsClipboard final : public IClipboard {
public:
    Result<std::string> ReadText() const override;
    Result<void> WriteText(std::string_view text) override;
    Result<std::vector<std::string>> GetFiles() const override;
    Result<void> SetFiles(const std::vector<std::string>& paths) override;
    // A screenshot posts CF_DIB (Snipping Tool / Win+PrtScn also offer PNG and
    // CF_BITMAP); reading CF_DIB is exact, so it is tried first.
    Result<ClipboardImage> ReadImage() const override;
    Result<void> WriteImage(const ClipboardImage& image) override;
};

} // namespace bps::platform
