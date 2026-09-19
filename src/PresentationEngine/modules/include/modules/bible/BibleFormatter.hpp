#pragma once

// Bible Formatter (docs/specs/24 §Formatting). Formatting rules belong to the
// engine, never the UI: paragraph mode, verse-per-line, headings, footnotes,
// and red-letter markers are produced here from the canonical model.

#include "core/common/Common.hpp"
#include "modules/bible/BibleTypes.hpp"

#include <string>

namespace bps::bible {

class BibleFormatter {
public:
    // Formats the passage for `ref` in the requested mode.
    static Result<std::string> Format(const BibleVersion& bible,
                                      const PassageRef& ref,
                                      const FormatOptions& opts = {});
};

} // namespace bps::bible
