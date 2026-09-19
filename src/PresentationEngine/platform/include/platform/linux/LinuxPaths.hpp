#pragma once

// Linux (XDG) PAL backend for the paths subsystem.

#include "../IPaths.hpp"

namespace bps::platform {

class LinuxPaths final : public IPaths {
public:
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

} // namespace bps::platform
