#include "modules/rendering/RenderObject.hpp"

#include <chrono>
#include <format>

namespace bps::rendering {

std::string CountdownObject::FormatRemaining(int64_t nowMs) const {
    const auto* c = Countdown();
    int64_t remaining = c ? (c->targetEpochMs - nowMs) : 0;
    if (remaining < 0) remaining = 0;
    const int64_t totalSec = remaining / 1000;
    const int64_t mm = (totalSec / 60) % 60;
    const int64_t ss = totalSec % 60;
    const int64_t hh = totalSec / 3600;
    const std::string fmt = c ? c->format : std::string("MM:SS");
    if (fmt.contains("HH"))
        return std::format("{:02}:{:02}:{:02}", hh, mm, ss);
    return std::format("{:02}:{:02}", mm, ss);
}

} // namespace bps::rendering
