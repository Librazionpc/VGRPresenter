#pragma once

#include <ctime>

namespace bps {

// The calendar breakdown of `t` in the machine's local time zone, thread-safe. The C library spells this
// differently on Windows (localtime_s, arguments swapped) and everywhere else (localtime_r); that
// difference lives HERE once instead of at every place that prints a clock.
inline std::tm LocalTime(std::time_t t) {
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

} // namespace bps
