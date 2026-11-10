# Duplication audit — the rest of the tree

The same detector that cleaned the LiveOutput service (`docs/long_file_audit.md`, "The repetition
audit") run over everything else: `qml/**/*.qml|*.cpp|*.hpp|*.h|*.mm` and `src/**`, 768 files.
**560 runs of 5+ identical lines** were found; the clusters worth acting on have been fixed (below),
and what remains is listed at the end with the reason it was left.

## Method

Normalise each line (trim, collapse whitespace) and ignore the lines that legitimately repeat
everywhere: blank, a lone `{` / `}` / `};` / `);`, `#include`, `#pragma`, a `//` line, an access
label (`public:` / `signals:`), `namespace` / `using namespace`, and a bare `return;` / `break;` /
`continue;`. Slide a 5-line window over what is left; a window whose text appears in more than one
place (and in fewer than 6, so a shared idiom is not counted as a copy) is extended both ways into
the maximal identical run, and runs are reported longest-first with overlaps dropped.

A run of identical lines is not automatically a defect — two switch arms or two fixture builders can
be correct as they are. The ranking is what matters: a copy that will drift is worth a helper, a
shared shape is not.

## Fixed

### 1. The platform classes repeated the same 18 service accessors — 18 lines x 3 OSes

`WindowsPlatform`, `LinuxPlatform` and `MacOSPlatform` each carried the identical block (the
macOS backend, stubs for everything, has since been deleted outright — see
`docs/platform/qt_first_platform.md`)

```cpp
IFilesystem& Filesystem() override { return filesystem_; }
IPaths& Paths() override { return paths_; }
... 16 more ...
INotifications& Notifications() override { return notifications_; }
```

Now one template, `platform/include/platform/PlatformAccessors.hpp`:

```cpp
template <class Derived>
class PlatformAccessors : public IPlatform { ... IFilesystem& Filesystem() override { return self().filesystem_; } ... };
```

Each backend is `class WindowsPlatform final : public PlatformAccessors<WindowsPlatform>` with
`friend class PlatformAccessors<WindowsPlatform>` and keeps its own `Info` / `OsName` / `OsVersion` /
`Arch` / `Sample` / `PollChanges`. CRTP because the members are per-OS types; the accessor bodies
exist once, and a backend that loses a member is a compile error in one place instead of a silent
divergence between three copies. **~54 lines removed, and the next OS gets the accessors for free.**

### 2. Modal chrome, copied across six source pickers — 125 lines removed

`BackgroundColorModal`, `BusSourceModal`, `CameraSourceModal`, `ClockSourceModal`,
`ShapeSourceModal` and `StylePickerModal` each carried the same 25-line close button (28x28
`Rectangle`, hover `ColorAnimation`, ✕ `Text`, 28x28 `MouseArea`). Now
`qml/components/ModalCloseButton.qml`, instantiated in four lines per modal as
`ModalCloseButton { id: closeBtn; onClicked: root.cancelled() }` — the id is kept so the existing
`closeBtn.height` references in each header row still resolve.

`TemplatePickerModal` keeps its own: it uses an `IconGlyph` glyph rather than the ✕ `Text`, so it
is not the same control.

### 3. Two QML files carried the same JS — 25 lines per function pair

`layoutClassOf(key)` (16 lines) existed in `StylesScreen.qml` and `RoutingScreen.qml`;
`syncVideoMeters()` (9 lines) existed in `MediaLibraryPane.qml` and `AudioVideoScreen.qml`. Both now
live in `qml/settings/ScreenHelpers.qml`, a `pragma Singleton` registered in CMake like `Theme` /
`StageGrid` (`set_source_files_properties(... QT_QML_SINGLETON_TYPE true)`), and each screen keeps
its local function as a one-line delegate so no call site changed.

`syncVideoMeters` became `ScreenHelpers.syncVideoMeters(wanted, held)` returning the set now held,
so the singleton does not reach into a caller's property.

### 4. `OutputMonitorTile` and `OutputWindow` — 4 copies of the overlay layer

Both files carried an 11-line `Repeater` for under-slide overlays and the same 11 lines again for
over-slide ones. Now `qml/components/OverlayLayer.qml`, with `underSlide` choosing the placement
group (which is also the draw order) and `blacked` nulling the model. Child order in both files is
unchanged: under → content → frame image → over.

### 5. Channel gain/trim writes in the two list models — 4 sites

`AudioInputListModel` (8 channels) and `VideoSourceListModel` (2 channels) each grew a `QList<qreal>`
to the channel index, compared with `qFuzzyCompare` and wrote, once for gains and once for trims.
Now `channeltrim::setChannelValue(values, channel, value, absent)` in `models/ChannelTrim.h`,
returning false when the value was already there so the caller still skips its write and its
`dataChanged` emission. The per-model channel bounds stay where they are.

## Separate log implementations, merged

The tree had **three** writers into the same stream, which is why the same event could read
differently in the console and in `engine.log`, or appear in only one of them:

| writer | what it did |
|---|---|
| `bps::Logger` + `FileSink` | the real engine log (`logs/engine.log`), one format |
| `ForwardToEventBus` (main.cpp) | printed every Qt message raw to stderr, then forwarded **only** warnings/criticals to the logger |
| `Logger::Log`'s boot fallback | printed a *different* shape (`[INFO][UI] msg`, no timestamp or thread) before the pump existed |

Consequences: `qInfo()` diagnostics (the models' roster lines), QML `console.log`, `qCInfo`
progress lines and the `[SELFTEST-STYLEBG-ID]` `fprintf`s never reached `engine.log` at all, and a
warning reached the console **twice** — once raw, once formatted.

Now there is one pipeline:

* **one mapping** — `LogLevelNameForQt(type)` maps every `QtMsgType` to the log's own level name
  (`debug` / `info` / `warning` / `error`), so a QML `console.log`, a `qInfo()` and an
  `EngineBridge::write` are indistinguishable downstream;
* **one formatter** — the boot fallback builds a `LogRecord` and calls the same `Format()`, so a line
  logged before the logger starts reads exactly like one logged after;
* **one console writer** — `ConsoleSink` is now installed alongside `FileSink` (it never was: the app
  installed its own sink before `Initialize()`, which skips its default sinks); the handler no longer
  prints to stderr itself;
* **one level policy** — the global gate sits at `Debug` and each sink filters itself: the console
  keeps everything (a self-test reads the app's console back), `engine.log` starts at `Info`. That is
  why the app's `qInfo()` lines now reach the file while `console.log` stays console-only by level,
  not by path.

The `[SELFTEST-STYLEBG-ID]` `fprintf`s in `OutputListModel` / `StyleListModel` are now
`EngineBridge::write(..., "StyleBgId", ...)` — same pipeline, same file, one format.
`CrashHandler` keeps its own `crash.log`: it is written while the process is dying, from a signal
handler, and must not depend on a pump thread — a different requirement, not a duplicate.

## Left in place, with the reason

| cluster | evidence | why |
|---|---|---|
| `IconGlyph.qml` | 137 lines over 19 runs, within one file | it is a table of SVG path fragments; the repeats are glyphs that share curves, and merging them obscures the table |
| `tests_styles.cpp`, `tests_rendering.cpp` | 87 + 10 lines | fixture setup; tests read better literal, and the policy has been to leave them |
| `Vfs.hpp:44` / `:71`, `TextMatching.hpp:134` / `:175` | 10 / 11 lines | interface overrides and two intentionally parallel matchers ("same rule, other input") |
| `ScriptureService.cpp` <-> `TheTableService.cpp` | 40 lines over 6 runs | parallel features (Bible and Table) whose shared part is a thread-runner + token + log shape. Sharing it means a common base over two 900-line services; the token and latest-wins semantics would have to be preserved exactly, and that is a change to how the app warms and searches, not a dedupe |
| `SongEngine.cpp`, `MediaEngine.cpp`, `RecordingScreen.qml`, `AudioVideoScreen.qml` and the other within-one-file runs | 35–118 lines each | the same screen repeating a `Row { }` / `Text { }` block for its third and fourth tab. The fix is the screen decomposition already planned in `long_file_audit.md` §4, not a new helper |

## Verification

Each C++ change was compiled with the project's own flags (`-std=c++26`, full include set,
`-fsyntax-only`) — `WindowsPlatform.cpp` stands in for the shared accessor template, and both list
models compile against the new `ChannelTrim.h` helper. Each touched `.qml` was parsed with
`qmllint`; the module import warning is expected outside a built module tree, and no file reported a
syntax or name error.
