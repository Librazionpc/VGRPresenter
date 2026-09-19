#pragma once

// Windows PAL backend for the paths subsystem (SHGetKnownFolderPath + shell
// known folders). Implementations live in platform/windows/WindowsPaths.cpp.

#include "platform/IPaths.hpp"

namespace bps::platform {

class WindowsPaths final : public IPaths {
public:
    std::string ExecutableDir() const override;
    std::string CurrentWorkingDir() const override;
    std::string ConfigDir() const override;    // %APPDATA%\bps
    std::string CacheDir() const override;     // %LOCALAPPDATA%\bps\cache
    std::string PluginDir() const override;    // %LOCALAPPDATA%\bps\plugins
    std::string AssetDir() const override;     // %LOCALAPPDATA%\bps\assets
    std::string LogDir() const override;       // %LOCALAPPDATA%\bps\logs
    std::string TempDir() const override;
    std::string DownloadsDir() const override;
    std::string DocumentsDir() const override;
    std::string DesktopDir() const override;
    std::string UserDataDir() const override;  // %LOCALAPPDATA%\bps
    std::string AppDataDir() const override;   // %APPDATA%\bps
};

} // namespace bps::platform
