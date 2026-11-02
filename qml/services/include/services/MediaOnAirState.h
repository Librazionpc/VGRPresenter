#pragma once

#include <string>

namespace services::media_on_air {
// The fit an on-air take is framed with. Empty = no take (readers default it).
extern std::string g_mediaFitOnAir;

extern bool g_mediaHasAlphaOnAir;
} // namespace services::media_on_air