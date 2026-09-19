#pragma once

// OutputRouter (docs/specs/18 §Output System). The only path frames travel:
// Renderer → one frame → OutputRouter → Outputs. One frame is shared; each
// output scales/crops it locally (adding a monitor never doubles rendering
// cost). Contains pure, unit-testable scaling math (Native/Fit/Fill/Stretch/
// Letterbox/Crop/PixelPerfect).

#include "core/common/Common.hpp"
#include "modules/display/DisplayTypes.hpp"
#include "modules/rendering/RenderOutputs.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace bps::display {

class OutputRouter {
public:
    // --- Output registry -------------------------------------------------------
    Result<void> AddOutput(const Output& output);
    Result<void> RemoveOutput(std::string_view id);
    Result<Output*> GetOutput(std::string_view id);
    std::vector<Output> Outputs() const;
    size_t Count() const;
    Result<void> SetEnabled(std::string_view id, bool enabled);
    Result<void> SetTransform(std::string_view id, const OutputTransform& transform);
    void Clear();

    // --- Pure scaling math (docs/specs/18 §4) -----------------------------------
    // Computes the destination rectangle on the device after applying the
    // output's scaling mode to the source frame region.
    static rendering::Rect ComputeDestRect(const DisplayDevice& device,
                                           const OutputTransform& transform,
                                           const rendering::Frame& frame);

    // --- Frame routing -----------------------------------------------------------
    using DeviceResolver = std::function<const DisplayDevice*(std::string_view deviceId)>;
    using Sink = std::function<void(const Output&, const rendering::Rect&,
                                    const rendering::Frame&)>;
    // Distributes ONE frame to every enabled output with a bound, connected
    // device (resolved through `devices`). Each output receives its dest rect
    // on the device plus the scaled frame.
    void RouteFrame(const rendering::Frame& frame, const DeviceResolver& devices,
                    const Sink& sink);

private:
    mutable std::mutex mutex_;
    std::vector<Output> outputs_;
};

} // namespace bps::display
