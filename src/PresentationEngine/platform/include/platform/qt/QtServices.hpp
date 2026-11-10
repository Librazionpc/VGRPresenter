#pragma once

// THE QT PLATFORM SERVICES — one implementation for every operating system.
//
// Each class here implements a PAL interface (platform/I*.hpp) with a Qt API that already hides
// the OS: QStandardPaths, QLocale, QLibrary, QThread. The engine never sees these types — it keeps
// talking to IPaths / ILocale / ILibrary / IThreading — so the production core stays standard C++
// and a headless build can install a different backend.
//
// Where Qt has no API for something (pinning a thread to a core, naming the calling thread), the
// service holds the NATIVE adapter's interface and forwards exactly that call. See
// docs/platform/qt_first_platform.md.
//
// STAGE 1 of the cage: paths, locale, libraries, threading (plus the Qt filesystem in
// QtFilesystem.hpp). DISPLAYS are deliberately NOT here yet — QScreen may only be read on the
// interface thread, and that hand-off is being designed together with the HDMI device-feed work
// rather than bolted on now (the native monitor adapter stays in place).

#include "platform/ILibrary.hpp"
#include "platform/ILocale.hpp"
#include "platform/IPaths.hpp"
#include "platform/IThreading.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class QLibrary;

namespace bps::platform {

// ---- Paths: QStandardPaths + the app's own folder layout -------------------------------------
// The folder LAYOUT (…/bps, …/bps/cache, the VGR_PROFILE_DIR override) is the app's contract with
// the data already on people's disks, so it is reproduced exactly; only the per-OS ROOTS come from
// QStandardPaths.
class QtPaths final : public IPaths {
public:
    QtPaths() = default;
    std::string ExecutableDir() const override;
    std::string CurrentWorkingDir() const override;
    std::string ConfigDir() const override;
    std::string CacheDir() const override;
    std::string PluginDir() const override;
    std::string AssetDir() const override;
    std::string LogDir() const override;
    std::string TempDir() const override;
    std::string DownloadsDir() const override;
    std::string DocumentsDir() const override;
    std::string DesktopDir() const override;
    std::string UserDataDir() const override;
    std::string AppDataDir() const override;

};

// ---- Locale: QLocale::system() ----------------------------------------------------------------
class QtLocale final : public ILocale {
public:
    LocaleInfo Current() const override;
    std::string Language() const override;
    std::string Country() const override;
};

// ---- Dynamic libraries: QLibrary --------------------------------------------------------------
class QtLibrary final : public ILibrary {
public:
    QtLibrary();
    ~QtLibrary() override;
    Result<Handle> Load(std::string_view path) override;
    Result<void> Unload(Handle handle) override;
    Result<void*> Symbol(Handle handle, std::string_view name) override;
    bool IsLoaded(Handle handle) const override;
    std::string LastError() const override;
    Result<Handle> Reload(Handle handle, std::string_view path) override;

private:
    void SetLastError(std::string_view message);

    mutable std::mutex mutex_;
    // The handle handed out IS the QLibrary's address; the map owns it.
    std::unordered_map<Handle, std::unique_ptr<QLibrary>> loaded_;
    std::string lastError_;
};

// ---- Threading: QThread, with the native adapter for what Qt has no API for -------------------
class QtThreading final : public IThreading {
public:
    explicit QtThreading(IThreading& native) : native_(native) {}
    unsigned HardwareConcurrency() const override;
    Result<void> SetCurrentThreadName(std::string_view name) override;
    std::string CurrentThreadName() const override;
    uint64_t CurrentThreadId() const override;
    // Qt has no thread-affinity API: these two are the native adapter's.
    Result<void> SetThreadAffinity(std::thread& thread, unsigned cpu) override;
    Result<void> SetThreadAffinityById(uint64_t nativeThreadId, unsigned cpu) override;
    Result<void> SetCurrentThreadPriority(int priority) override;

private:
    IThreading& native_;
};

} // namespace bps::platform
