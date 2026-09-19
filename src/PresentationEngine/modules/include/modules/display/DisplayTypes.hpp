#pragma once

// Display & Output Engine (Phase 7, docs/specs/18). The engine no longer thinks
// in terms of "Monitor 1 / Monitor 2 / Projector" — it thinks in terms of
// Outputs. A monitor is just one type of output; tomorrow an output could be
// OBS, NDI, an LED wall, a web browser, a mobile app, a virtual display, a
// recording, a screenshot, AI vision, or a remote stage display. This module
// contains display logic only — never WinUI/HWND/XAML/dialogs.

#include "core/common/Common.hpp"
#include "modules/rendering/RenderTypes.hpp"

#include <string>
#include <vector>

namespace bps::display {

// --- Scaling (docs/specs/18 §4) -------------------------------------------------
enum class ScalingMode : int {
    Native = 0,       // 1:1 at native device resolution
    Fit,              // scale to fit inside the target rect (letterbox)
    Fill,             // scale to cover the target rect (crop overflow)
    Stretch,          // exact target rect, ignore aspect ratio
    Letterbox,        // fit + letterbox bars
    Crop,             // fill + crop overflow
    PixelPerfect,     // integer scale factor only
};

inline const char* ToString(ScalingMode m) {
    switch (m) {
        case ScalingMode::Native:      return "Native";
        case ScalingMode::Fit:         return "Fit";
        case ScalingMode::Fill:        return "Fill";
        case ScalingMode::Stretch:     return "Stretch";
        case ScalingMode::Letterbox:   return "Letterbox";
        case ScalingMode::Crop:        return "Crop";
        case ScalingMode::PixelPerfect:return "PixelPerfect";
    }
    return "Unknown";
}

// --- Output kinds (logical destinations, never physical windows) ---------------
enum class OutputKind : int {
    Audience = 0,   // projector / main screen
    Stage,          // confidence monitor
    Preview,        // operator preview
    Thumbnail,
    Stream,
    Recording,
    Screenshot,
    Virtual,        // headless / virtual display
    Remote,         // remote app / web
    Custom,
};

inline const char* ToString(OutputKind k) {
    switch (k) {
        case OutputKind::Audience:   return "Audience";
        case OutputKind::Stage:      return "Stage";
        case OutputKind::Preview:    return "Preview";
        case OutputKind::Thumbnail:  return "Thumbnail";
        case OutputKind::Stream:     return "Stream";
        case OutputKind::Recording:  return "Recording";
        case OutputKind::Screenshot: return "Screenshot";
        case OutputKind::Virtual:    return "Virtual";
        case OutputKind::Remote:     return "Remote";
        case OutputKind::Custom:     return "Custom";
    }
    return "Unknown";
}

// --- Output lifecycle state -----------------------------------------------------
enum class OutputState : int {
    Off = 0,
    Starting,
    Running,
    Failed,
    Lost,           // device disconnected
};

inline const char* ToString(OutputState s) {
    switch (s) {
        case OutputState::Off:      return "Off";
        case OutputState::Starting: return "Starting";
        case OutputState::Running:  return "Running";
        case OutputState::Failed:   return "Failed";
        case OutputState::Lost:     return "Lost";
    }
    return "Unknown";
}

// --- Color (docs/specs/18 §6) ---------------------------------------------------
enum class ColorProfile : int {
    SRgb = 0,
    Rec709,
    Gamma,
    SafeColor,
    Custom,
};

// --- A display device: a physical/virtual output the engine can route to --------
struct DisplayDevice {
    std::string id;            // stable id ("eDP-1", "\\.\DISPLAY1", "virtual-aud")
    std::string name;          // friendly name
    int x = 0;                 // virtual-desktop origin
    int y = 0;
    int width = 0;             // native resolution
    int height = 0;
    int refreshRateHz = 0;     // 0 = unknown
    int dpi = 96;
    int orientation = 0;       // degrees: 0 / 90 / 180 / 270
    bool primary = false;
    bool connected = true;
    bool hdrSupported = false;
    std::string provider;      // owning provider name
    bool virtual_ = false;     // virtual/headless device
};

// --- Output transform: how a logical output maps onto a device ------------------
struct OutputTransform {
    ScalingMode scaling = ScalingMode::Fit;
    rendering::Rect sourceRect;        // region of the frame to present (empty = full)
    int x = 0;                         // destination origin on the device
    int y = 0;
    int width = 0;                     // destination size (0 = device size)
    int height = 0;
    ColorProfile color = ColorProfile::SRgb;
    float gamma = 1.0f;
};

// --- Logical output --------------------------------------------------------------
struct Output {
    std::string id;            // stable id ("audience", "stage-2")
    OutputKind kind = OutputKind::Audience;
    std::string name;
    std::string displayId;     // bound device id ("" = unbound)
    OutputTransform transform;
    bool enabled = true;
    bool autoRestore = true;   // re-attach when the display returns
    OutputState state = OutputState::Off;
    uint64_t framesDelivered = 0;
    uint64_t framesLost = 0;
};

// --- Profiles (docs/specs/18 §Display Profiles) ---------------------------------
struct LayoutItem {
    std::string outputId;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct DisplayProfile {
    std::string name;               // "Church Main Hall", "Conference Hall"
    std::vector<Output> outputs;
    std::vector<LayoutItem> layout; // optional visual layout
};

} // namespace bps::display
