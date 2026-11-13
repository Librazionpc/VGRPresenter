# The platform layer — Qt-first OS services, platform-free engine

Status: **Qt is the single implementation** of every OS-generic service —
filesystem, paths, locale, dynamic libraries, threading. The old per-OS paths /
locale / library classes, the `std::filesystem` backend and the macOS backend (a
file of stubs) are gone; there is no "build for Windows this way" branch left to
maintain. Native adapters remain only for devices and displays, behind the same
PAL interfaces.

**Platform support status:** Windows remains the production target. Linux and macOS use Qt Multimedia for microphone capture, program and routed output rendering, decoded media audio taps, audio and camera device discovery, and asynchronous camera preview. Targeted display preview is available on Linux/X11 and macOS with the Qt FFmpeg backend; Linux/Wayland remains unavailable because Qt cannot target the PAL monitor ID there. Linux and macOS support audio-only PCM/WAV recording; video recording and output loopback meters remain unavailable outside Windows. These limits are exposed through runtime capabilities and the Recording screen. Media audio and screen capture require Qt's FFmpeg multimedia backend. A successful configure or compile is not by itself a production support claim.

## Why

Three hand-written backends for the same OS service is three places to fix one
bug, and they drift. Qt already owns the hard part of that mapping:

* `QFile`/`QDir` reach the operating system's wide APIs, so a non-ASCII path is
  not re-encoded through the user's ANSI code page;
* `QStandardPaths` knows every platform's config/cache/documents roots;
* `QLibrary` knows the loader and its error strings;
* `QLocale` knows formats, territory and text direction;
* `QThread` knows the machine's worker count.

The production engine never learns about any of this: it holds `IFilesystem&`,
`IPaths&`, ... and composes scenes in standard C++. It stays usable headless and
independent of Qt Quick.

## The rule

1. **Qt serves every OS-generic service** — one implementation, no matter which
   platform is being brought up.
2. **A native adapter remains only where Qt has no adequate API** — camera and
   screen capture, NDI stream taps/loopback, raw input, sockets, power, notifications,
   dialogs, and the display topology a live production machine needs. It is the
   only place an OS header may appear.
3. **The engine core never does "OS work" itself** — no `<windows.h>`, no
   `<unistd.h>`, no Qt header to reach a file, a device or a display. It asks
   the PAL. `tools/check_platform_boundary.mjs` fails the build otherwise.
4. **QML never talks to the PAL directly.** It talks to the services layer
   (`qml/services`, `qml/models`), where the threading and GUI-thread rules live.

## Layout

```
src/PresentationEngine/
├── platform/
│   ├── include/platform/          the interfaces — the ONLY seam the engine sees
│   │   ├── IFilesystem.hpp, IPaths.hpp, ILocale.hpp, ILibrary.hpp, IThreading.hpp, …
│   │   └── PlatformServices.hpp   names the concrete classes (all Qt)
│   ├── qt/                        QtFilesystem, QtPaths, QtLocale, QtLibrary, QtThreading
│   ├── common/                    Timer (OS-free)
   windows/ linux/ macos/   native platform adapters
│   └── Platform.cpp               the one platform-specific symbol the core links
├── modules/                       production engine — standard C++, PAL only
└── tests/unit/tests_platform*.cpp the service contract suites
```

Each OS platform class composes the PAL; only its device adapters are
OS-specific now:

```cpp
    // WindowsPlatform / LinuxPlatform / MacOSPlatform — the split is only in the device adapters
    PlatformPaths paths_;                    // QtPaths
    PlatformFilesystem filesystem_;          // QtFilesystem
    PlatformLibrary library_;                // QLibrary
    PlatformLocale locale_;                  // QLocale
    WindowsThreading nativeThreading_;       // naming/affinity: no Qt API
    PlatformThreading threading_{nativeThreading_};
```

## Service matrix

| PAL service | Backend | Why |
|---|---|---|
| `IFilesystem` | **Qt** (`QtFilesystem`) | UTF-8 ↔ native names once; verified atomic replace; one implementation |
| `IPaths` | **Qt** (`QtPaths`) | `QStandardPaths` is the platform's own answer; the app's folder layout (`…/bps`, `VGR_PROFILE_DIR`) is reproduced exactly |
| `ILocale` | **Qt** (`QtLocale`) | `QLocale::system()` owns formats, territory, direction |
| `ILibrary` | **Qt** (`QtLibrary`) | `QLibrary` owns the loader, errors and symbol lookup |
| `IThreading` | **Qt** for counts/ids; native for naming, affinity, priority | Qt has no API for those three |
| `ITimer` | common (`TimerImpl`) | `std::chrono`, no event loop; `QTimer` needs one and belongs to the UI layer, which uses it throughout `qml/` |
| `IMonitor` | native today; `QtMonitor` written, **not in the build** | `QScreen` may only be read on the interface thread; that hand-off lands with the HDMI output work |
| `IVideo`, `IAudio` | Qt Multimedia camera/audio capture and render | Device discovery is portable; callback I/O feeds the existing mixer contracts |
| `IInput` | native adapters | Global input discovery/monitoring has platform permission limits |
| `IPower`, `IClipboard`, `IEnvironment`, `IDialogs`, `INotifications` | native adapters | Small OS surfaces Qt exposes only in ways that do not fit a headless engine |
| `ISocket`, `INetwork` | native adapters (`PosixSocket`, WinSock, `getifaddrs`) | The engine's I/O threads have no `QEventLoop`; `QTcpSocket` would need one per connection |
| `IProcess` | native adapters | `QProcess` is used in `qml/services` where a `QEventLoop` exists; the PAL contract needs an explicit wait/timeout story that is not Qt's |

## Platform coverage

| Operating system | PAL composition | Production media status |
|---|---|---|
| Windows | Windows native adapters + shared Qt services | Production target; audio render/capture, camera, and screen capture are implemented |
| Linux | Linux native adapters + shared Qt services + Qt Multimedia audio/camera path | Microphone capture, audio render, media/NDI stream taps, camera preview, X11 screen preview and PCM/WAV audio recording are implemented; Wayland screen capture, loopback meters and video recording remain unavailable |
| macOS | Apple frameworks + shared Qt services + POSIX process/sockets + Qt Multimedia audio/camera path | Microphone capture, audio render, media/NDI stream taps, camera preview, targeted screen preview and PCM/WAV audio recording are implemented; loopback meters and video recording remain unavailable |

A configured or compiled Linux/macOS target is not a claim of production feature parity. Runtime validation on each OS audio stack and device permissions is still required.

**Not Qt, deliberately:** `AppSettings` is the app's own descriptor-driven JSON store
(that descriptor list *is* the settings screen), not `QSettings`; `QSettings` appears
only for Windows shell integration in `qml/services/SettingsService.cpp`. Logging uses
`core/logging/QtMessagePolicy` to turn Qt messages into user notices; the logger
itself is the engine's.

`ExecutableDir()` is the one path Qt cannot answer before an application object
exists; there it returns the working directory instead of reaching for an OS API.
Anything that needs the real executable is an application and has one.

## Enforcement

`tools/check_platform_boundary.mjs`:

* `core/`, `modules/`, `interfaces/` — no OS header, no Qt header.
* `qml/` — no OS header (Qt is its framework).
* `platform/` is exempt by construction; `tests/unit` is exempt because the
  contract suites pin backend behaviour directly.

Existing sites are in `tools/platform_boundary_baseline.json` (path + rule +
reason) and are counted on every run; a new one fails the build. Wired as
`platform_boundary_check` (part of `all`, a dependency of the app and the unit
tests) and as `ctest -R platform_boundary`.

## Testing a service

* `build/bps_unit_tests platform` — the contract suites
  (`tests_platform.cpp`, `tests_platform_contract.cpp`): UTF-8 paths, append,
  atomic replace with no `.tmp` left behind, metadata, recursive find, enumerate,
  temp files, symlinks, the watcher firing once per change and not after
  `CancelWatch`; listing spelling, remove-what-is-gone, permission bits, the
  profile folder layout and the locale fields.
* `tools/dev_cycle.sh --build-only` — build plus the four source guards
  (lock scope, GUI-thread, settings keys, platform boundary).

## What comes next

1. **Displays (next, for Windows)** — a `QScreen`-backed `IMonitor` whose reads happen on the
   interface thread and reach the engine as a snapshot, keeping the native
   adapter for the topology details Qt does not expose. Linux/X11 and macOS
   targeted screen preview use `QScreenCapture`; macOS resolves stable
   CoreGraphics display UUIDs through Qt screen geometry and declares the
   system screen-capture usage.
2. **Filesystem watching** — evaluate `QFileSystemWatcher` where a `QEventLoop`
   exists in `qml/`, keeping the polling watcher for engine threads (see
   `platform/README.md`, house rules).
3. **Screen capture on Wayland** — Linux/X11 and macOS targeted screen preview
   use Qt Multimedia through the existing `IVideo` frame contract. Qt's current
   Wayland path cannot target the PAL monitor ID, so targeted capture remains
   unavailable there.

## Decisions recorded (what the cage does and does not claim)

### The engine core is not Qt-free, and that is deliberate for text and images
`tools/platform_boundary_baseline.json` lists five Qt-header sites in the engine: `QtThread.hpp`,
`BlockMeta.hpp`, `TextEngine.hpp/.cpp` and `SceneBuilder.cpp`. `QtThread` exists so engine threads are
real `QThread`s (a thread Qt did not start is *adopted* the first time it touches Qt — that was the exit
crash and the 20 s quit freeze). The text engine lays out and rasterises text with `QFont` /
`QTextLayout` / `QPainter` / `QImage`: that is one font stack and one set of metrics on every OS, which
is the point of using Qt here, and replacing it would mean a second text engine that must match it
glyph for glyph. So `engine_core` links `Qt6::Gui` (not only Core); the rule "no Qt header to do *OS
work*" still holds — files, paths, devices and displays go through the PAL — and the five sites stay on
the ledger with that reason instead of being hidden. New Qt includes in `core/` or `modules/` still
fail the guard.

### Display identity: one key, resolved where it is used
The engine **stores** an output's screen name (`OutputStore`) and never joins it to a platform monitor;
the interface resolves it against `QScreen::name()`. Screen *sources* use the PAL monitor id, and the
picker's label is resolved back to that id (`EngineBridge::startScreenPreview`). So the two namespaces
never have to be equal:

| OS | PAL monitor id (stable key) | Label the picker shows |
|---|---|---|
| Windows | `\.\DISPLAY1` (== `QScreen::name()`) | the same |
| Linux | DRM connector, e.g. `eDP-1` (== `QScreen::name()` on X11) | the same |
| macOS | CoreGraphics UUID | `Built-in · 3024×1964` / `Display 2 · 1920×1080` (CoreGraphics data only — AppKit screens are main-thread-only and this is called from engine threads) |

`QtMonitor` (the `QScreen`-backed snapshot, written, **not built**) becomes worthwhile when the HDMI
output work needs live hot-plug and per-display scale from one source; until then it stays out of the
build rather than adding a second source of truth.

### A taken camera or screen reaches the output on the GPU
`LiveInputSurface.qml` is the one way a taken camera/screen is drawn (output windows and monitor tiles),
exactly as `LiveMediaSurface.qml` is for a media file. `LiveOutputService` runs one worker per taken
input (`startInputFeed`): it waits for the tap's frame counter to move, copies the newest picture once
into a `QVideoFrame`, and hands it to every attached `VideoOutput`. No JPEG, no per-consumer decode, no
interface-thread work per frame. A QML scene on the software backend (no GPU) and NDI inputs keep the
`image://videopreview` path, which each parent still draws as the fallback.

### Packaging
**One command on every OS: `tools/make_installer.sh`.** It reads the version from `kAppVersion` in
`SettingsService.cpp` (never by hand) and prints the same `INSTALLER: PASS` / `INSTALLER: FAIL <stage>`
verdict everywhere:

| Host | Output | Flow |
|---|---|---|
| Windows | `dist/VGRPresenter-Setup-<ver>.exe` | `make_portable.sh --release` → seed → Inno Setup (unchanged) |
| Linux | `dist/VGRPresenter-Setup-<ver>-linux-<arch>.AppImage` (+ `.tar.gz`) | hands over to `make_installer_unix.sh` → Release build → seed → `package_linux.sh` (linuxdeploy + Qt plugin, Qt `multimedia` deployed explicitly) |
| macOS | `dist/VGRPresenter-Setup-<ver>-macos-<arch>.dmg` | same, → `package_macos.sh` (`macdeployqt`, ad-hoc or Developer-ID signing, dmg) |

Shared steps live in `tools/lib/unix_release.sh` (version, artifact name, seed). The seed is the same on
every OS: design catalogs generated by `bps_cli --dump-catalogs` and this machine's configuration
snapshot beside the executable (`assets/settings.shipped.json`). Windows lays the seed data down in its
installer; a Linux/macOS package carries it in `share/vgrpresenter/seed-data` (AppImage) or
`Contents/Resources/seed-data` (.app) and the app lays it down itself on a fresh profile
(`seedFirstRun` in `src/main.cpp`: never over an existing file, nothing on a profile that has a
settings.json). Notarisation needs credentials and is a separate step. `package.yml` runs the same
command in CI; `platform-ci.yml` builds all three OSes and runs the platform contract tests and the
source guards on each.

### Still not built (stated, not hidden)
* **Windows-native device paths that Qt could own.** `WindowsVideo`/`WindowsAudio` (live-mode camera taps,
  WASAPI loopback) stay for the latency they buy; the per-OS clipboard / dialogs / notifications /
  network / environment / input implementations duplicate what `QClipboard`, the Qt Quick dialogs,
  `QSystemTrayIcon`, `QNetworkInformation` and `QScreen` already do once.

### Decisions recorded (Windows-only audit)
* **Picture decoding is Qt on Linux/macOS** (`platform/qt/QtFrameSource.cpp`): `QImageReader` for stills
  (EXIF-rotated, downscaled at decode), `QMediaPlayer` + a bare `QVideoSink` for video posters,
  cover art from the tags for audio. Before it, those OSes answered `Unsupported` to every JPEG/WebP
  background, video poster and Media-pane thumbnail. Pinned by the `platform / frame-source-stills` test.
* **Thread priority goes through the PAL** (`IThreading::SetCurrentThreadPriority`); the Table indexer
  used a Win32 call behind `Q_OS_WIN`.
* **"Launch at login" works on every OS** (registry / LaunchAgent / XDG autostart).
* **The QRhi GPU backend is written but not yet verified on Linux/macOS hardware** (see the decision below):
  run `bps_unit_tests rhi-backend` there and read the `[gpu] frame match` line.

### Decision: one GPU backend for every OS (QRhi)
`platform/qt/QtRhiBackend.cpp` is the engine's GPU backend on Qt's rendering hardware interface —
Direct3D 11 on Windows, Metal on macOS, OpenGL then Vulkan on Linux — behind the same
`IGraphicsBackend` seam the D3D11 backend uses. It is a command-for-command port of
`D3D11Backend.cpp`: the same premultiplied offscreen frame, the same edge-snapped quads, the same
second pass for the CSS blend modes and the same copy-plus-scissor pass for blur / brightness /
contrast / saturation / opacity. The shaders (`platform/qt/shaders/engine.{vert,frag}`) are GLSL,
compiled by Qt Shader Tools at build time into one `.qsb` per stage holding every API's format
(GLSL ES 100 / 120 / 150, HLSL 5.0, MSL 1.2, SPIR-V); a fix to a shader or a pipeline reaches all
three OSes.

* **Selection.** `render.backend` / the hardware-acceleration switch resolve to `"gpu"` = "the best this OS
  has": Direct3D 11 on Windows (the proven path, still the default there), the QRhi backend elsewhere,
  software if neither can start. `"rhi"` forces the QRhi backend on any OS (including Windows, to test
  the shared path where a developer is); `"directx11"` forces D3D11. `BPS_RHI_API=d3d11|metal|vulkan|gl`
  picks the API underneath. Flip Windows to QRhi by changing one branch in `RenderEngine::SelectBackend`
  once `rhi-backend` has run clean there.
* **Threads.** A QRhi (and an OpenGL context above all) is thread-affine, and the engine calls its
  backend from the render thread, the texture uploader and the thumbnail workers, so every call is
  marshalled onto ONE dedicated GPU thread that owns the QRhi. The OpenGL offscreen surface is created
  on the GUI thread (a switch of the hardware-acceleration setting arrives from a worker).
* **Orientation.** Commands are top-left origin; the vertex shader emits GL clip space and
  `clipSpaceCorrMatrix()` adapts it per API. Only OpenGL stores a render target bottom-up, which the
  shader (frame copies) and `Readback` undo, keyed on `isYUpInFramebuffer()`.
* **Tests.** `GpuBackendMatchesSoftware` renders one scene on the CPU rasterizer and on the GPU backend
  and demands every pixel within +-3: `d3d11-backend` runs it against `"gpu"` (this OS's best),
  `rhi-backend` against `"rhi"`. A bare test process has no Qt GUI application, which OpenGL/Vulkan need;
  set `BPS_TEST_GUI=1` (and `QT_QPA_PLATFORM=offscreen` on a headless box) to give it one. Without a
  device the tests assert the software fallback instead.
* **Not changed.** The GPU is still opt-in (the switch is off on a fresh install, on every OS); frames
  still come back to the CPU through a readback, exactly as with D3D11, because the outputs, NDI and
  recording all take CPU frames.

### Decision: recording (every OS) and output metering on Linux/macOS (Qt)
* **Recording.** The recording engine was already portable (frames come from the render engine's on-air
  buffer, audio from the mixer tap); the file writer is now ONE Qt implementation on Windows, Linux and
  macOS (the separate Media Foundation writer, which produced unplayable files, was removed). `QtRecordingContainer`
  (`platform/qt/QtRecordingContainer.cpp`) is an `IRecordingContainer` over `QMediaRecorder` fed by
  `QVideoFrameInput` / `QAudioBufferInput` (Qt 6.8+) — MP4, MKV, MOV, WebM, and M4A / MP3 / FLAC where the
  FFmpeg backend encodes them, with hardware encoders (VideoToolbox, VAAPI, NVENC) chosen by Qt. The
  containers registered are the ones the machine's backend reports it can encode, so the Recording
  screen's lists show what works; `PlatformCapabilities::videoRecording` is that same probe.
  Deliberately no format hint on the two inputs (`QVideoFrameInput()` / `QAudioBufferInput()`): with a hint, Qt 6.11's
  FFmpeg backend on Windows never signals ready and the file ends up with no tracks. `Close()` drains the queues before
  stopping (a slow encoder, MP3 especially, would otherwise be cut short). The output path is made absolute.
  The Qt objects live on a private thread with an event loop; the pump's writes copy and queue, so a
  slow encoder drops frames instead of stalling the pump. Pinned by `platform / qt-recording` (a real
  one-second take; run with `BPS_TEST_GUI=1`, plus `QT_QPA_PLATFORM=offscreen` headless).
* **Output metering.** The program mix is PLAYED by the app, so its level is measured in that render
  stream (`MeterRender` in `QtAudio.cpp`; `OutputLevels()` reads it) — no operating-system loopback
  (WASAPI loopback / a PulseAudio monitor source / a macOS tap are three mechanisms for one number),
  and it reads what this app plays rather than every other program's sound. `audioLoopbackCapture`
  therefore means "output metering available" and is true on all three OSes.
* **One version source.** CMake reads `kAppVersion` from `SettingsService.cpp` for the project
  version, the Windows file-properties resource and the macOS bundle (they repeated `0.1.3`).

### Decision: the last Windows-only / OS-only spots
* **File pickers on Linux** (`platform/qt/QtDialogs.cpp`): the PAL dialogs used to shell out to `zenity` /
  `kdialog`, so a machine without one could not choose a file (the app imports everything through those
  pickers). They now come from Qt's platform theme (xdg-desktop-portal / GTK / KDE helper — the same
  dialogs Qt Quick's FileDialog shows, no Widgets, nothing to install); zenity stays as the fallback.
  macOS already shows `NSOpenPanel`/`NSAlert`; Windows the COM dialogs.
* **Screen capture on Wayland**: reported available. Qt's FFmpeg backend asks the portal, which shows its
  own "choose what to share" dialog (a Wayland client cannot name a monitor); with no portal the start
  fails and the error reaches the UI. X11 and macOS still capture the targeted monitor.
* **Backend switch race fixed** (`RenderEngine::SelectBackend`): it replaced `backend_`, rebound the GPU
  resources and cleared the texture cache WITHOUT the frame lock, under a live frame — the intermittent
  crash on switching hardware rendering on. The swap now happens between two frames; slow device
  creation still happens before the lock.
* **`LocalTime()`** (`core/common/LocalTime.hpp`) replaces six copies of the `localtime_s`/`localtime_r` split.
* **Found by compiling the Qt platform sources** (they are not part of a Windows build): a call to
  `TargetScreen` before its declaration and a `const char* + const char*` in the audio fingerprint —
  both would have stopped the first Linux/macOS build. Every `platform/qt/*.cpp` now passes `g++
  -fsyntax-only`.

### Still not built
* **The per-OS clipboard / notifications / network / environment / input classes** still exist per OS. The
  app does not call the clipboard or notification interfaces at all, and the rest work; folding them onto
  `QClipboard`, `QSystemTrayIcon`, `QNetworkInformation` is cleanup, not a missing feature.
* **`QtMonitor`** (the `QScreen`-backed display list) is written but not in the build; the native
  monitor classes still serve the HDMI output's display identity.

### Decision: the last per-OS services — Qt first, native for the remainder
* **Clipboard — one class** (`QtClipboard`): text, file lists and images through `QClipboard` on the GUI
  thread, on all three OSes. The `NSPasteboard`, `CF_*` and `xclip`/`wl-paste` implementations are gone.
* **Dialogs — Linux** (`QtDialogs`, above); macOS and Windows already show their native dialogs.
* **Network, environment, input, notifications** (`QtComposed.cpp`): each is now answered by ONE Qt
  implementation, and the per-OS class (kept as `native*_` in each platform) is consulted only for what Qt
  cannot say. Where Qt has nothing (no GUI application yet, an empty list) the native answer is returned
  unchanged, so no service reports less than before.

  | Service | From Qt (every OS) | Still native (Qt has no API) |
  |---|---|---|
  | Network | adapters, MACs, IPv4/IPv6, hostname, reachability (`QNetworkInterface`, `QNetworkInformation`) — adds the Qt Network module | default gateway, DNS servers, inbound-firewall rule, proxy from the environment |
  | Environment | OS name/version/arch, hostname, user, locale, time zone, cores (`QSysInfo`, `QLocale`, `QTimeZone`) | CPU model, GPU name (the adaptive hardware profile reads it), total RAM, OS build id |
  | Input | keyboards, mice, touch, pen (`QInputDevice`) | gamepads and `/dev/input` device nodes (Qt 6 has no gamepad API) |
  | Notifications | the platform theme's tray icon — Windows balloon, Notification Center, freedesktop D-Bus (no Widgets, no QtDBus module) | the old per-OS toast when the theme has no tray |

### Still not built
* **`QtMonitor`** (the `QScreen`-backed display list) is written but not in the build; the native
  monitor classes still serve the HDMI output's display identity.
