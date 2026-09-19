# Platform Abstraction Layer (PAL)

> **Phase 2 of the architecture brief.** The PAL is the **only** place in the
> engine that knows which operating system it is running on. Everything the
> engine does with the OS — files, paths, time, threads, processes, shared
> libraries, monitors, audio devices, the network, power, the clipboard, the
> environment and the locale — goes through one of the interfaces below.
> Feature modules and core managers never call OS APIs directly and never
> include OS headers (`windows.h`, `<pthread.h>`, `<unistd.h>`, `dlfcn.h`…).

## 1. Purpose

- **Isolate** the engine from the OS so the same core runs on Linux (WSL),
  Windows and macOS without modification.
- **Testability**: every subsystem is an interface with a Linux backend and
  unit tests; a mock backend can stand in for the OS in tests.
- **Swappability**: a new OS (or a headless stub for CI) is a new backend
  behind the same interfaces — no engine code changes.

## 2. Responsibilities & design rules

- The PAL provides **only OS services** — never business logic.
- **Never expose native handles** (`HWND`, `Display*`, `HDC`…) outside the PAL.
  Library handles are opaque `void*`; monitor ids are stable strings.
- Prefer standard C++ (`std::filesystem`, `std::thread`, `std::chrono`) where
  it meets the need; use OS APIs only where necessary (monitor enumeration,
  thread naming/affinity, process control, dynamic loading).
- No code outside `platform/` may branch on `_WIN32` / `__linux__` /
  `__APPLE__`; the compile-time tags live in `platform/include/platform/OsTag.hpp`
  (`platform::kCompileOs`, `platform::kCompileArch`) and are the only macros
  the core may read (Kernel uses them for `BuildInfo`). Grep-verified: **zero**
  OS-gated `#if` outside the PAL.
- **Folder structure (DoD §folder)**: all headers live under
  `platform/include/platform/` (never beside their `.cpp`); implementations
  stay in `platform/{linux,windows,macos,common}/`. CMake adds
  `platform/include` to the include path.
- Every PAL function returns `Result<T>` (never raw `bool` for failure) with
  code, message, module, `nativeError` (errno / `WSAGetLastError`) and — in
  debug builds — a captured call-site `stack` (see §8).

## 3. The subsystem interfaces (`platform/`)

| Interface | File (under `platform/include/platform/`) | Services (Phase 2) |
|---|---|---|
| `IPlatform` | `IPlatform.hpp` | Facade: `Name/Info/OsName/OsVersion/Arch`, 18 subsystem accessors, `Sample()` telemetry, `PollChanges()` OS events |
| `IFilesystem` | `IFilesystem.hpp` | Read/Write (text+binary), Append, Move/Rename/Copy, Remove/RemoveAll, CreateDirectory(s), Enumerate, recursive `FindFiles`, `Metadata` (size, mtime, permissions), `CreateTempFile`, symlinks, polling `Watch` |
| `IPaths` | `IPaths.hpp` | Executable, CWD, Config, Cache, Plugin, Asset, Log, Temp, Downloads, Documents, Desktop, UserData, AppData |
| `ITimer` | `ITimer.hpp` | Monotonic ns (`NowNs`), UTC epoch ms (`WallClockMs`), `UtcNow`/`LocalNow` clock parts, `SleepMicros`, `Stopwatch`, engine-time `EngineNow` |
| `IThreading` | `IThreading.hpp` | `HardwareConcurrency`, thread naming, OS thread id, CPU affinity, `SetCurrentThreadPriority` (nice / `SetThreadPriority`) |
| `IProcess` | `IProcess.hpp` | Start/Wait/ExitCode/Kill/Terminate/IsRunning, `Restart`, `CurrentProcessId`, environment get/set |
| `ILibrary` | `ILibrary.hpp` | Load/Unload/Reload/Symbol, `IsLoaded`, `LastError` (dlopen / `LoadLibraryW` errors) |
| `IMonitor` | `IMonitor.hpp` | Enumerate monitors (id, name, resolution, refresh, DPI, orientation, HDR, primary, connected), `Primary`, `DefaultMonitorId` |
| `IAudio` | `IAudio.hpp` | Enumerate devices (id, name, input/output, default, sample rate, channels), `DefaultOutput`, `DefaultInput`, `Fingerprint` (hot-plug) |
| `INetwork` | `INetwork.hpp` | Hostname, adapters (name/MAC/IPv4/IPv6/up/loopback), `IpAddress`, `Gateway`, `DnsServers`, `Proxy`, `InternetAvailable` |
| `IPower` | `IPower.hpp` | `Current()` — battery percent, on-battery, charging |
| `IClipboard` | `IClipboard.hpp` | `ReadText`/`WriteText`, `GetFiles`/`SetFiles` (file lists: `text/uri-list`, `CF_HDROP`) |
| `IEnvironment` | `IEnvironment.hpp` | OS, version, arch, build number, CPU model, GPU name, RAM, cores, username, hostname, locale, timezone |
| `ILocale` | `ILocale.hpp` | Language, country, date/time/number formats, currency, RTL flag |
| `ISocket` | `ISocket.hpp` | TCP transport: `ISocket` (Send/Receive/ReceiveTimeout), `ISocketListener` (poll-based Accept, `Port`), `ISocketFactory` (Connect/Listen) — the **only** socket API the core uses (`core/ipc`) |
| `IInput` | `IInput.hpp` | Input device discovery (keyboard/mouse/touch/pen/gamepad) with ids, names, device paths; `HasKeyboard`/`HasPointer` |
| `IDialogs` | `IDialogs.hpp` | Open-file / save-file / folder pickers (cancel → `nullopt`), message + question boxes (modal) |
| `INotifications` | `INotifications.hpp` | Desktop notifications (fire-and-forget, best-effort) |

Every interface documents purpose, responsibilities, error conditions, thread
safety and platform notes in its header (DoD §23). The DoD's interface names
map to ours as: `IFilesystem`≈FileSystem, `IThreading`≈IThread,
`IMonitor`≈IDisplay, `IAudio`≈IAudioDevice, `ILibrary`≈ILibraryLoader,
`ISocket`≈(IPC transport), `IEnvironment`, `ILocale`.

## 4. Access pattern

The Kernel creates the backend once at boot (step 12) and installs it:

```cpp
auto platform = std::shared_ptr<platform::IPlatform>(platform::CreatePlatform());
platform::PlatformAccessor::Install(platform);           // global access
ResourceManager::Instance().SetPlatform(platform);       // telemetry consumer
```

Any engine system can then route through the PAL without dependency plumbing:

```cpp
auto& fs = platform::PlatformAccessor::Get().Filesystem();
auto text = fs.ReadText("/path/to/file.json");           // Result<std::string>
auto lib = platform::PlatformAccessor::Get().Library();
```

`PlatformAccessor` lazily creates the default backend on first use, so unit
tests and pre-Kernel code work without explicit setup.

## 5. Backends

- **`platform/common/`** — OS-agnostic implementations built on the C++
  standard library: `FilesystemImpl` (`std::filesystem`), `TimerImpl`
  (`std::chrono`).
- **`platform/linux/`** — Linux: XDG paths, pthread naming/affinity/priority,
  fork/exec/waitpid, dlopen, sysfs DRM monitors, ALSA PCM streams
  (`/proc/asound/pcm`), getifaddrs + `/proc/net/route` + `/etc/resolv.conf`,
  `/sys/class/power_supply`, clipboard tools (wl-copy/xclip/xsel,
  `text/uri-list`), `/proc/bus/input/devices` (input), zenity/notify-send
  (dialogs/notifications), environment/locale from `LANG`/`/proc`.
- **`platform/windows/`** — complete Win32 backend for all 18 subsystems:
  SHGetKnownFolderPath paths, `SetThreadDescription` naming, `SetThreadPriority`,
  `CreateProcessW`, `LoadLibraryW`, `EnumDisplayMonitors`, winmm
  waveIn/waveOut, iphlpapi, `GetSystemPowerStatus`, `CF_UNICODETEXT` +
  `CF_HDROP`, Raw Input, Common Item Dialog + `MessageBoxW`, PowerShell
  balloon, Winsock (`WindowsSocket`). Requires a Windows host to
  compile-verify (developed on Linux/WSL — Conformance §15.1).
- **`platform/macos/`** — interface stubs; adding macOS = implementing the
  same interfaces (DoD §24).

**Architecture coverage** (DoD §24 — every target, nobody left behind): the
PAL is pointer-width and endianness agnostic, with **no host-architecture
assumptions anywhere** (no `-march=native`, no x86-isms; Win32-specific size
hazards — affinity masks, socket `int` lengths — are guarded):

| OS | Architectures |
|---|---|
| Windows | **x86 (32-bit)**, **x64**, **ARM64** (and 32-bit ARM) |
| Linux | x86_64, **aarch64**, **32-bit ARM (armv7)**; riscv64 ready |
| macOS | arm64 + x86_64 (interface stubs) |

`platform::CompileArch()` / `kCompileArch` report the canonical architecture
(`x86_64` · `x86` · `arm64` · `arm` · `riscv64`), `kCompileBits` the pointer
width, and `CanonicalArch()` normalizes OS-reported strings (`aarch64` →
`arm64`, `armv7l` → `arm`, `AMD64` → `x86_64`) so `Arch()` and
`EnvironmentInfo.arch` are identical on every platform. One documented
exception: Linux reports the **kernel** arch via `uname`, so a 32-bit build on
64-bit hardware reports `x86_64`/`arm64` (the binary is still `x86`/`arm` per
`kCompileArch`). Cross-compiling is a standard CMake toolchain-file operation
(`-DCMAKE_TOOLCHAIN_FILE=...`).

## 6. OS events → engine events

`IPlatform::PollChanges()` is drained by the Kernel's platform watcher (a 1s
`ScheduleEvery` task) and published onto the Event Bus:

| OsEventType | Event topic |
|---|---|
| `MonitorConnected` | `platform.monitor_connected` |
| `MonitorDisconnected` | `platform.monitor_disconnected` |
| `ResolutionChanged` | `platform.monitor_resolution_changed` |
| `PowerChanged` | `platform.power_changed` |
| `BatteryLow` | `platform.battery_low` |
| `Sleep` / `Wake` | `platform.sleep` / `platform.wake` |
| `NetworkChanged` | `platform.network_changed` |
| `DeviceConnected` / `DeviceRemoved` | `platform.device_connected` / `platform.device_removed` |
| `LocaleChanged` | `platform.locale_changed` |
| `ClipboardChanged` | `platform.clipboard_changed` |

The Linux backend currently *detects* monitor hot-plug, resolution changes,
power/battery transitions and **audio device hot-plug** (via `IAudio::Fingerprint`);
the remaining event types are emitted by future backends through the same
pipeline.

## 7. Threading model

- Every PAL subsystem is **thread-safe**: state is mutex-guarded
  (`FilesystemImpl` watches, `LinuxProcess` pid table, `LinuxLibrary` handle
  set, `PlatformAccessor` magic-static slot).
- Interfaces document ownership in their headers. Callbacks fire **outside**
  locks (e.g. `Watch` change callbacks, so a callback may safely cancel its own
  watch).
- `PollChanges` is called on the scheduler thread; publishes happen on the
  Event Bus (itself thread-safe).

## 8. Error handling

Every public PAL function returns `Result<T>`/`Result<void>` (DoD §19). `Error`
carries: code, message, module, `nativeError` (errno / `WSAGetLastError`) when
 a backend surfaced one, and `stack` — a call-site backtrace captured in debug
builds by `platform::CaptureStack()` (glibc `backtrace`, populated by the
Filesystem and Socket backends; empty in release builds). Failure is never
signaled by exceptions across the PAL boundary.

## 9. Performance notes

- The PAL is a thin layer: hot paths are direct sysfs/proc reads and standard
  library calls; per-call overhead is nanoseconds.
- Targets (engineering goals, DoD §21): display enumeration < 100 ms on
  typical systems; timer precision sub-millisecond (steady-clock ns);
  dynamic library loading only at startup/plugin ops.
- `TestPalPerfStress` guards the targets with loose bounds (10k timer reads
  < 100 ms; 200 fs round trips < 5 s; 100 socket echoes < 15 s; 1000 pool
  tasks < 20 s) — regression guards, not benchmarks (DoD §22).

## 10. Testing

`tests/unit/main.cpp` → `TestPal` covers every public function of every
subsystem: filesystem CRUD/watch/metadata/symlinks/temp files, all paths,
timers, threading (+ priority), process lifecycle + restart, library
load/symbol/unload, monitors, audio (input/output/default/fingerprint), input
enumeration, network (gateway/DNS/proxy), power, clipboard (text + file
lists), environment, locale, error `nativeError`, the `ISocket` echo round
trip, and the platform events on the Event Bus. `TestPalPerfStress` adds
perf/stress coverage; `TestIpc`/`TestIpcLogStream` exercise the transport end
to end. Suite: **1655 checks, 0 failures**, AddressSanitizer-clean.

## 11. Future extensions (no redesign required)

Already behind interfaces, ready to deepen without redesign:

- **Input capture** — raw keyboard/mouse event streams and gamepad state
  polling on top of `IInput` (evdev / Raw Input event APIs).
- **Monitor depth** — orientation + HDR from DRM properties (Linux).
- **Audio depth** — sample-rate enumeration for idle devices via libasound;
  input-level meters.
- **Clipboard** — image and rich-text formats (already target-capable:
  `text/uri-list`, `CF_HDROP`).
- **Dialogs** — color / font pickers; **Notifications** — progress + alert
  variants with actions.
- **Windows compile validation** on a Windows host; **macOS backend** for all
  18 subsystems (same interfaces).
- **Transports** — WebSocket / gRPC / named-pipe IPC servers behind the same
  handler API, on the same `ISocket`/`ISocketListener` primitives.
