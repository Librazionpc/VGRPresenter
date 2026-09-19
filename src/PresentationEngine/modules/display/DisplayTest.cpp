#include "modules/display/DisplayTest.hpp"

#include "modules/display/OutputRouter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>

namespace bps::display {

namespace {

inline uint32_t Rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFFu << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(g) << 8) | r;
}

} // namespace

// ---------------------------------------------------------------------------
// TestPatternGenerator
// ---------------------------------------------------------------------------
rendering::Frame TestPatternGenerator::ColorBars(int width, int height) {
    rendering::Frame f;
    f.width = width;
    f.height = height;
    f.pixels.assign(static_cast<size_t>(width) * height, 0);
    static const uint32_t bars[7] = {
        Rgb(191, 191, 191),  // white
        Rgb(191, 191, 0),    // yellow
        Rgb(0, 191, 191),    // cyan
        Rgb(0, 191, 0),      // green
        Rgb(191, 0, 191),    // magenta
        Rgb(191, 0, 0),      // red
        Rgb(0, 0, 191),      // blue
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int bar = static_cast<int>(static_cast<double>(x) * 7 / width);
            bar = std::clamp(bar, 0, 6);
            f.pixels[static_cast<size_t>(y) * width + x] = bars[bar];
        }
    }
    return f;
}

rendering::Frame TestPatternGenerator::Gradient(int width, int height) {
    rendering::Frame f;
    f.width = width;
    f.height = height;
    f.pixels.assign(static_cast<size_t>(width) * height, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t v = static_cast<uint8_t>(std::clamp(
            static_cast<int>(255.0 * y / std::max(1, height - 1)), 0, 255));
        uint32_t px = Rgb(v, v, v);
        for (int x = 0; x < width; ++x)
            f.pixels[static_cast<size_t>(y) * width + x] = px;
    }
    return f;
}

rendering::Frame TestPatternGenerator::Checkerboard(int width, int height, int cells) {
    rendering::Frame f;
    f.width = width;
    f.height = height;
    f.pixels.assign(static_cast<size_t>(width) * height, 0);
    const int cw = std::max(1, width / cells);
    const int ch = std::max(1, height / cells);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool black = ((x / cw) + (y / ch)) % 2 == 0;
            f.pixels[static_cast<size_t>(y) * width + x] = black ? Rgb(0, 0, 0) : Rgb(255, 255, 255);
        }
    }
    return f;
}

rendering::Frame TestPatternGenerator::Generate(TestPattern pattern, int width, int height,
                                                uint64_t frame) {
    switch (pattern) {
        case TestPattern::ColorBars:    return ColorBars(width, height);
        case TestPattern::Gradient:     return Gradient(width, height);
        case TestPattern::Checkerboard: return Checkerboard(width, height);
        case TestPattern::SolidWhite: {
            rendering::Frame f;
            f.width = width;
            f.height = height;
            f.pixels.assign(static_cast<size_t>(width) * height, Rgb(255, 255, 255));
            f.frame = frame;
            return f;
        }
        case TestPattern::SolidBlack: {
            rendering::Frame f;
            f.width = width;
            f.height = height;
            f.pixels.assign(static_cast<size_t>(width) * height, Rgb(0, 0, 0));
            f.frame = frame;
            return f;
        }
        case TestPattern::SolidRed: {
            rendering::Frame f;
            f.width = width;
            f.height = height;
            f.pixels.assign(static_cast<size_t>(width) * height, Rgb(255, 0, 0));
            f.frame = frame;
            return f;
        }
        case TestPattern::SolidGreen: {
            rendering::Frame f;
            f.width = width;
            f.height = height;
            f.pixels.assign(static_cast<size_t>(width) * height, Rgb(0, 255, 0));
            f.frame = frame;
            return f;
        }
        case TestPattern::SolidBlue: {
            rendering::Frame f;
            f.width = width;
            f.height = height;
            f.pixels.assign(static_cast<size_t>(width) * height, Rgb(0, 0, 255));
            f.frame = frame;
            return f;
        }
        case TestPattern::FrameCounter: {
            // Solid green with a red square whose position encodes the frame
            // number — visually confirms live frame updates.
            rendering::Frame f = Checkerboard(width, height, 8);
            f.frame = frame;
            const int bx = static_cast<int>(frame % static_cast<uint64_t>(std::max(1, width / 8)));
            const int by = static_cast<int>((frame / static_cast<uint64_t>(std::max(1, width / 8))) %
                                            static_cast<uint64_t>(std::max(1, height / 8)));
            for (int y = by * 8; y < std::min(height, by * 8 + 16); ++y)
                for (int x = bx * 8; x < std::min(width, bx * 8 + 16); ++x)
                    f.pixels[static_cast<size_t>(y) * width + x] = Rgb(255, 0, 0);
            return f;
        }
    }
    rendering::Frame f;
    f.width = width;
    f.height = height;
    f.pixels.assign(static_cast<size_t>(width) * height, 0);
    return f;
}

// ---------------------------------------------------------------------------
// DisplayTester
// ---------------------------------------------------------------------------
DisplayTestReport DisplayTester::TestConnectivity(const std::vector<DisplayDevice>& devices) {
    DisplayTestReport report;
    report.devices = devices;
    for (const auto& d : devices) {
        bool ok = d.connected && d.width > 0 && d.height > 0;
        std::string detail = d.connected
                                 ? std::format("{}x{} @ {}Hz", d.width, d.height,
                                               d.refreshRateHz)
                                 : "disconnected";
        report.Add("device.connected", d.id, ok, detail);
    }
    if (devices.empty())
        report.Add("device.enumerate", "(none)", false, "no display devices found");
    report.Finish();
    return report;
}

DisplayTestReport DisplayTester::TestFrameDelivery(OutputRouter& router, DeviceResolver devices,
                                                   const rendering::Frame& probe) {
    DisplayTestReport report;
    int delivered = 0;
    router.RouteFrame(probe, devices,
                      [&](const Output& output, const rendering::Rect& rect,
                          const rendering::Frame& scaled) {
                          bool ok = scaled.width == static_cast<int>(rect.width) &&
                                    scaled.height == static_cast<int>(rect.height) &&
                                    !scaled.pixels.empty();
                          report.Add("output.delivery", output.id, ok,
                                     std::format("dest {}x{}", static_cast<int>(rect.width),
                                                 static_cast<int>(rect.height)));
                          ++delivered;
                      });
    if (delivered == 0)
        report.Add("output.delivery", "(all)", false,
                   "no enabled output received the probe frame");
    report.Finish();
    return report;
}

DisplayTestReport DisplayTester::TestScaling(const OutputRouter& router) {
    DisplayTestReport report;
    (void)router;
    // Pure geometry check on a representative device + frame.
    DisplayDevice dev;
    dev.id = "test-dev";
    dev.width = 1920;
    dev.height = 1080;
    rendering::Frame f;
    f.width = 1920;
    f.height = 1080;
    f.pixels.assign(1920 * 1080, 0);

    // Stretch fills exactly.
    {
        OutputTransform t;
        t.scaling = ScalingMode::Stretch;
        auto r = OutputRouter::ComputeDestRect(dev, t, f);
        bool ok = static_cast<int>(r.width) == 1920 && static_cast<int>(r.height) == 1080;
        report.Add("scaling.stretch", "fit-16:9", ok, "1920x1080");
    }
    // Fit letterboxes a 4:3 source into 16:9 (dest narrower than device).
    {
        OutputTransform t;
        t.scaling = ScalingMode::Fit;
        rendering::Frame f43;
        f43.width = 1024;
        f43.height = 768;
        auto r = OutputRouter::ComputeDestRect(dev, t, f43);
        double aspect = static_cast<double>(r.width) / r.height;
        bool ok = std::fabs(aspect - 1024.0 / 768.0) < 0.01 && r.width <= 1920;
        report.Add("scaling.fit", "4:3->16:9", ok,
                   std::format("{}x{}", static_cast<int>(r.width), static_cast<int>(r.height)));
    }
    // PixelPerfect keeps an integer scale.
    {
        OutputTransform t;
        t.scaling = ScalingMode::PixelPerfect;
        rendering::Frame fs;
        fs.width = 800;
        fs.height = 600;
        auto r = OutputRouter::ComputeDestRect(dev, t, fs);
        bool ok = static_cast<int>(r.width) % 800 == 0 && static_cast<int>(r.height) % 600 == 0;
        report.Add("scaling.pixelperfect", "800x600->1080p", ok,
                   std::format("{}x{}", static_cast<int>(r.width), static_cast<int>(r.height)));
    }
    report.Finish();
    return report;
}

DisplayTestReport DisplayTester::RunAll(DeviceResolver devices, OutputList outputs,
                                        OutputRouter* router,
                                        std::function<Result<size_t>()> restore) {
    DisplayTestReport report;
    // 1. Connectivity: collect the devices referenced by outputs (deduped).
    std::vector<DisplayDevice> bound;
    if (devices) {
        for (const auto& o : outputs()) {
            if (auto* d = devices(o.displayId)) {
                bool seen = false;
                for (const auto& e : bound)
                    if (e.id == d->id) seen = true;
                if (!seen) bound.push_back(*d);
            }
        }
    }
    auto conn = TestConnectivity(bound);
    report.devices = conn.devices;
    report.checks.insert(report.checks.end(), conn.checks.begin(), conn.checks.end());
    report.passed += conn.passed;
    report.failed += conn.failed;

    // 2. Scaling math.
    auto scale = TestScaling(*router);
    report.checks.insert(report.checks.end(), scale.checks.begin(), scale.checks.end());
    report.passed += scale.passed;
    report.failed += scale.failed;

    // 3. Frame delivery with a color-bar probe.
    if (router) {
        auto probe = TestPatternGenerator::ColorBars(320, 180);
        auto delivery = TestFrameDelivery(*router, devices, probe);
        report.checks.insert(report.checks.end(), delivery.checks.begin(),
                             delivery.checks.end());
        report.passed += delivery.passed;
        report.failed += delivery.failed;
    }

    // 4. Recovery: force-restore assignments and expect no crash + count.
    if (restore) {
        auto r = restore();
        if (r.ok()) {
            report.Add("output.recovery", "(lost)", true,
                       std::format("{} output(s) restored", r.value()));
        } else {
            report.Add("output.recovery", "(lost)", false, r.error().message);
        }
    }

    report.outputs = outputs();
    report.Finish();
    return report;
}

} // namespace bps::display
