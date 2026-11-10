#pragma once

// ---------------------------------------------------------------------------
// THE PLATFORM SERVICES — Qt-first, and now Qt-only for everything it covers.
//
// The engine only ever holds an IFilesystem/IPaths/... reference (see
// platform/IPlatform.hpp); this header names the concrete classes the OS
// platform classes compose. The old per-OS paths/locale/library backends are
// gone: Qt is the single implementation of those services on every platform,
// so there is no "build for Windows this way" branch left to maintain.
//
// THE RULE (docs/platform/qt_first_platform.md):
//   1. Qt serves every OS-generic service: filesystem, paths, locale, dynamic
//      libraries, threading.
//   2. A native adapter remains ONLY where Qt has no adequate API — camera and
//      screen capture, NDI stream taps/loopback, raw input, sockets, power,
//      notifications, dialogs — behind the same PAL interfaces.
//   3. The engine core never includes an OS header or a Qt header to do "OS
//      work": it asks the PAL. tools/check_platform_boundary.mjs enforces it.
// ---------------------------------------------------------------------------

#include "platform/qt/QtFilesystem.hpp"
#include "platform/qt/QtServices.hpp"

namespace bps::platform {

using PlatformFilesystem = QtFilesystem;
using PlatformPaths = QtPaths;
using PlatformThreading = QtThreading;
using PlatformLibrary = QtLibrary;
using PlatformLocale = QtLocale;

} // namespace bps::platform
