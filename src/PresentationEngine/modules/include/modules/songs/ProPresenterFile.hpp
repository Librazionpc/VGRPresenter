#pragma once

#include "core/common/Common.hpp"
#include "modules/songs/SongTypes.hpp"

#include <string_view>

namespace bps::song {
Result<Song> ParseProPresenterFile(std::string_view content);
} // namespace bps::song