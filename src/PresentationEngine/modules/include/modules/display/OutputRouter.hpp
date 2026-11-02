#pragma once

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

    static rendering::Rect ComputeDestRect(const DisplayDevice& device,
                                           const OutputTransform& transform,
                                           const rendering::Frame& frame);

    // --- Frame routing -----------------------------------------------------------
    using DeviceResolver = std::function<const DisplayDevice*(std::string_view deviceId)>;
    using Sink = std::function<void(const Output&, const rendering::Rect&,
                                    const rendering::Frame&)>;
    void RouteFrame(const rendering::Frame& frame, const DeviceResolver& devices,
                    const Sink& sink);

private:
    mutable std::mutex mutex_;
    std::vector<Output> outputs_;
};
} // namespace bps::display