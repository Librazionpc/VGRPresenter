#pragma once

#include <string>

namespace bps::platform {
class IPaths {
public:
    virtual ~IPaths() = default;

    virtual std::string ExecutableDir() const = 0;
    virtual std::string CurrentWorkingDir() const = 0;
    virtual std::string ConfigDir() const = 0;
    virtual std::string CacheDir() const = 0;
    virtual std::string PluginDir() const = 0;
    virtual std::string AssetDir() const = 0;
    virtual std::string LogDir() const = 0;
    virtual std::string TempDir() const = 0;
    virtual std::string DownloadsDir() const = 0;
    virtual std::string DocumentsDir() const = 0;
    virtual std::string DesktopDir() const = 0;
    virtual std::string UserDataDir() const = 0;   // user-owned app data (XDG data on Linux)
    virtual std::string AppDataDir() const = 0;    // roaming app data (Xdg on Linux, %APPDATA% on Windows)
};
} // namespace bps::platform