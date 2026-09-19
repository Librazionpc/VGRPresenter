#pragma once

// Windows PAL backend for the clipboard subsystem: text (CF_UNICODETEXT) and
// file lists (CF_HDROP).

#include "platform/IClipboard.hpp"

namespace bps::platform {

class WindowsClipboard final : public IClipboard {
public:
    Result<std::string> ReadText() const override;
    Result<void> WriteText(std::string_view text) override;
    Result<std::vector<std::string>> GetFiles() const override;
    Result<void> SetFiles(const std::vector<std::string>& paths) override;
};

} // namespace bps::platform
