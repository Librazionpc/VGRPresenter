#pragma once

#include "../IClipboard.hpp"

namespace bps::platform {
class LinuxClipboard final : public IClipboard {
public:
    Result<std::string> ReadText() const override;
    Result<void> WriteText(std::string_view text) override;
    Result<std::vector<std::string>> GetFiles() const override;
    Result<void> SetFiles(const std::vector<std::string>& paths) override;
    Result<ClipboardImage> ReadImage() const override;
    Result<void> WriteImage(const ClipboardImage& image) override;
};
} // namespace bps::platform