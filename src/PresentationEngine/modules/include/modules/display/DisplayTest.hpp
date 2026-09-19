#pragma once

// DisplayTest (docs/specs/18 §Testing / §My Acceptance Test). A self-test
// subsystem for the Display & Output Engine: verifies that monitors/outputs are
// connected, that frames actually reach every output, that scaling modes
// produce correct rectangles, and that hot-plug + recovery work. Generates test
// patterns (color bars / gradient / checkerboard / solid) so an operator can
// visually confirm a projector or confidence monitor. Pure display logic — no UI.

#include "core/common/Common.hpp"
#include "modules/display/DisplayTypes.hpp"
#include "modules/rendering/RenderOutputs.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bps::display {

class OutputRouter;

// --- Test patterns (visual confirmation on a real screen) --------------------
enum class TestPattern : int {
    SolidWhite = 0,
    SolidBlack,
    SolidRed,
    SolidGreen,
    SolidBlue,
    ColorBars,      // SMPTE-style 75% bars
    Gradient,       // vertical black→white
    Checkerboard,   // 8x8 black/white
    FrameCounter,   // pattern encodes a frame number in the top-left block
};

inline const char* ToString(TestPattern p) {
    switch (p) {
        case TestPattern::SolidWhite:    return "SolidWhite";
        case TestPattern::SolidBlack:    return "SolidBlack";
        case TestPattern::SolidRed:      return "SolidRed";
        case TestPattern::SolidGreen:    return "SolidGreen";
        case TestPattern::SolidBlue:     return "SolidBlue";
        case TestPattern::ColorBars:     return "ColorBars";
        case TestPattern::Gradient:      return "Gradient";
        case TestPattern::Checkerboard:  return "Checkerboard";
        case TestPattern::FrameCounter:  return "FrameCounter";
    }
    return "Unknown";
}

// --- Per-check result ---------------------------------------------------------
struct CheckResult {
    std::string check;           // e.g. "device.connected"
    std::string target;          // e.g. "eDP-1"
    bool passed = false;
    std::string detail;          // e.g. "1920x1080 @60Hz"
};

// --- Full self-test report -----------------------------------------------------
struct DisplayTestReport {
    bool allPassed = false;
    size_t passed = 0;
    size_t failed = 0;
    std::vector<CheckResult> checks;
    std::vector<DisplayDevice> devices;
    std::vector<Output> outputs;

    void Add(std::string check, std::string target, bool ok, std::string detail = {}) {
        checks.push_back(CheckResult{std::move(check), std::move(target), ok, std::move(detail)});
        if (ok) ++passed; else ++failed;
    }
    void Finish() { allPassed = (failed == 0); }
};

// --- Test pattern generator (CPU-side RGBA8 frames) ---------------------------
class TestPatternGenerator {
public:
    // Renders a pattern into a frame of the given size.
    static rendering::Frame Generate(TestPattern pattern, int width, int height,
                                     uint64_t frame = 0);

    // Renders an SMPTE-style color-bar frame.
    static rendering::Frame ColorBars(int width, int height);
    // Vertical black→white gradient.
    static rendering::Frame Gradient(int width, int height);
    static rendering::Frame Checkerboard(int width, int height, int cells = 8);
};

// --- Display self-test runner --------------------------------------------------
// Runs every connectivity / routing / scaling / recovery check against a
// DisplayEngine-like view (device resolver + output router). Kept dependency-
// light: takes lambdas so it can test both the live engine and a mock.
class DisplayTester {
public:
    using DeviceResolver = std::function<const DisplayDevice*(std::string_view deviceId)>;
    using OutputList = std::function<std::vector<Output>()>;

    // Full suite: devices enumerated + connected, outputs bound + receiving
    // frames, scaling math, recovery of Lost outputs.
    static DisplayTestReport RunAll(DeviceResolver devices, OutputList outputs,
                                    OutputRouter* router,
                                    std::function<Result<size_t>()> restore);

    // Is every enumerated device marked connected with a valid resolution?
    static DisplayTestReport TestConnectivity(
        const std::vector<DisplayDevice>& devices);

    // Does a generated frame actually reach each output (via the router)?
    static DisplayTestReport TestFrameDelivery(
        OutputRouter& router, DeviceResolver devices, const rendering::Frame& probe);

    // Verify ComputeDestRect for every scaling mode on a 1920x1080 device.
    static DisplayTestReport TestScaling(const OutputRouter& router);
};

} // namespace bps::display
