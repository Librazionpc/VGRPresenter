#pragma once

#include "core/common/Common.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bps::platform {
struct ClipboardImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

class IClipboard {
public:
    virtual ~IClipboard() = default;

    virtual Result<std::string> ReadText() const = 0;
    virtual Result<void> WriteText(std::string_view text) = 0;

    // File lists (DoD §10): absolute paths, not file:// URIs. Best effort —
    // Unsupported when the clipboard provider has no file-list target.
    virtual Result<std::vector<std::string>> GetFiles() const = 0;
    virtual Result<void> SetFiles(const std::vector<std::string>& paths) = 0;

    virtual Result<ClipboardImage> ReadImage() const = 0;
    virtual Result<void> WriteImage(const ClipboardImage& image) = 0;
};
} // namespace bps::platform