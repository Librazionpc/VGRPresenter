# `platform/` — the PAL and its backends

The engine never talks to the operating system. It asks these interfaces; the classes
behind them change per platform. `docs/platform/qt_first_platform.md` is the why,
this file is the map.

```
platform/
├── include/platform/         the interfaces — the only ossature the engine sees
│   ├── IFilesystem.hpp IPaths.hpp ILocale.hpp ILibrary.hpp IThreading.hpp ITimer.hpp
│   ├── IMonitor.hpp IVideo.hpp IAudio.hpp IInput.hpp ISocket.hpp INetwork.hpp
│   ├── IProcess.hpp IPower.hpp IClipboard.hpp IEnvironment.hpp IDialogs.hpp
│   ├── IPlatform.hpp PlatformAccessors.hpp PlatformAccessor.hpp PlatformServices.hpp
│   ├── qt/                   Qt-backed services (one implementation, every OS)
│   ├── windows/              Win32 device & display adapters
│   ├── linux/                Linux device & display adapters
│   ├── macos/                Apple frameworks + shared service composition
│   └── common/               Qt-free helpers (Timer, PosixSocket)
├── qt/                       Qt services + QtMediaDevices camera discovery/preview
│                             (+ QtMonitor, written, not built yet — see below)
├── windows/                  Win32 adapters
├── linux/                    Linux adapters
├── macos/                    Apple framework adapters and platform composition
├── common/                   Timer.cpp, PosixProcess.cpp, PosixSocket.cpp
├── Platform.cpp              the only OS-specific symbol the core links
└── PlatformAccessor.cpp      lazily installs the default backend
```

## Who serves what

Generic OS services are **Qt, once, on every platform**:

| PAL | Backend | File |
|---|---|---|
| `IFilesystem` | `QtFilesystem` | `qt/QtFilesystem.cpp` |
| `IPaths` | `QtPaths` | `qt/QtPaths.cpp` |
| `ILocale` | `QtLocale` | `qt/QtLocale.cpp` |
| `ILibrary` | `QtLibrary` | `qt/QtLibrary.cpp` |
| `IThreading` (counts, ids) | `QtThreading` → native for naming/affinity/priority | `qt/QtThreading.cpp` |
| `ITimer` | `TimerImpl` (`std::chrono`, no event loop needed) | `common/Timer.cpp` |

Device, display and desktop services use native adapters where Qt cannot satisfy the
PAL's threading, permission or real-time contracts. Linux and macOS use shared Qt
Multimedia audio capture/render, media decode taps and camera discovery/preview.
Linux/X11 also uses Qt Multimedia for targeted display capture; Wayland capture and
macOS screen capture remain unavailable.

| PAL | Windows | Linux | macOS |
|---|---|---|---|
| `IMonitor` | `windows/WindowsMonitor.cpp` | `linux/LinuxMonitor.cpp` | `macos/MacOSPlatform.mm` (CoreGraphics) |
| `IVideo` | `windows/WindowsVideo.cpp` | `QtMediaDevices` via `linux/LinuxVideo.hpp`; camera and X11 screen preview | `QtMediaDevices`; camera preview, no screen capture |
| `IAudio` | `windows/WindowsAudio.cpp` | `QtAudioDevices` (Qt Multimedia capture, render, discovery) | `QtAudioDevices` (Qt Multimedia capture, render, discovery) |
| `IInput` | `windows/WindowsInput.cpp` | `linux/LinuxInput.cpp` | IOHID enumeration; global monitoring restricted |
| `IPower` | `windows/WindowsPower.cpp` | `linux/LinuxPower.cpp` | IOKit power sources |
| `IClipboard` | `qt/QtClipboard.cpp` (QClipboard, GUI thread) | same | same |
| `IEnvironment` | `windows/WindowsEnvironment.cpp` | `linux/LinuxEnvironment.cpp` | Qt, sysctl and uname |
| `IDialogs` | `windows/WindowsDialogs.cpp` | `linux/LinuxDialogs.cpp` | AppKit panels and alerts |
| `INotifications` | `windows/WindowsNotifications.cpp` | `linux/LinuxNotifications.cpp` | macOS notification center |
| `ISocket` / `INetwork` | `windows/WindowsSocket.cpp`, `WindowsNetwork.cpp` | `common/PosixSocket.cpp`, `linux/LinuxNetwork.cpp` | `common/PosixSocket.cpp`, `macos/MacOSPlatform.mm` |
| `IProcess` | `windows/WindowsProcess.cpp` | `common/PosixProcess.cpp` | `common/PosixProcess.cpp` |

`MacOSPlatform` composes the shared Qt services with Apple frameworks and common
POSIX process/socket services. Qt Multimedia supplies microphone capture, program
and routed output rendering, decoded media audio taps, and camera preview. NDI
audio arrives through the PAL's push method. Output loopback metering and screen
capture remain unavailable on macOS and are not advertised.

## Adding a Qt service (the pattern to copy)

1. Put the class in `include/platform/qt/QtServices.hpp` (or its own header for
   anything as big as the filesystem), implementation in `qt/`.
2. Alias it in `include/platform/PlatformServices.hpp`.
3. Compose it in `windows/WindowsPlatform.hpp` / `linux/LinuxPlatform.hpp`; keep the
   native adapter member only where the Qt class delegates to it.
4. Delete the per-OS duplicates in the same change — a service has one owner.
5. Add it to `CMakeLists.txt` (`BPS_QT_PLATFORM_SOURCES` or the applicable platform source list) and to the contract suite in
   `tests/unit/tests_platform*.cpp`.
6. `tools/check_platform_boundary.mjs` must stay green: no OS or Qt header outside
   `platform/`, no OS header in `qml/`.

## House rules

* Qt classes here run on whatever thread calls the PAL, so they must not touch
  widgets, windows or `QScreen`. `QtMonitor` is the exception by construction: it
  reads `QScreen` only on the interface thread and is **not in the build yet** — it
  lands with the display/HDMI work.
* `QtFilesystem` watches by polling (500 ms) instead of `QFileSystemWatcher`: the
  engine has no event loop on the threads that watch media folders, and the polling
  loop is the same on every OS with no handle limits.
* Every service is thread-safe and returns `Result<T>`; callbacks fire outside locks.
