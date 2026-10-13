#pragma once

// PAL clipboard subsystem (Phase 2): text, file lists and bitmaps through a
// common interface. On headless Linux this is best-effort via the system
// clipboard tools (xclip / wl-copy); returns Unsupported when none are available.

#include "core/common/Common.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bps::platform {

// A clipboard BITMAP as plain RGBA8 — tightly packed, row-major, top row first,
// four bytes per pixel. That is the render engine's own RgbaImage pixel
// convention, so a screenshot on the clipboard reaches an import (or the
// canvas) with no decode step and no image codec in the platform layer.
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

    // BITMAPS — what the clipboard actually holds after a screenshot
    // (Win+Shift+S, PrtScn and the Snipping Tool all post a bitmap, often with
    // no text at all), so text-only clipboard support reports "nothing to
    // import" for the most ordinary copy on the machine.
    //
    // ReadImage returns Err::NotFound when the clipboard holds no image (the
    // common text-only case — a caller asks "is there a picture?" without
    // treating the answer as a failure) and Err::Unsupported when the backend
    // has no image target at all. WriteImage publishes RGBA8 as a platform
    // bitmap, so a capture taken in the app can be pasted into any other
    // program.
    virtual Result<ClipboardImage> ReadImage() const = 0;
    virtual Result<void> WriteImage(const ClipboardImage& image) = 0;
};

} // namespace bps::platform
