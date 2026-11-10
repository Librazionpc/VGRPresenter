#pragma once

#include "platform/qt/QtMediaDevices.hpp"

namespace bps::platform {
// Linux uses Qt Multimedia's portable device ids and asynchronous camera
// preview path. QtVideoDevices also provides targeted X11 screen preview when
// running on xcb; Wayland cannot target the PAL's monitor identifiers here.
using LinuxVideo = QtVideoDevices;
} // namespace bps::platform
