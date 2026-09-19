#pragma once

#include "interfaces/IService.hpp"

namespace bps {

// External plugin manifest. Sidecar <name>.json next to the native library.
struct PluginManifest {
    std::string id;
    Version version;
    std::string author;
    std::vector<std::string> dependencies;
    Version requiredCoreVersion;
    std::vector<std::string> capabilities;
    std::string libraryPath;   // relative or absolute path to .so/.dll/.dylib
    int abiVersion = 1;        // must match kPluginAbiVersion (docs/specs/09)
};

inline constexpr int kPluginAbiVersion = 1;

// Plugin interface (docs/specs/09). Loaded from shared libraries via the
// PluginManager; the native ABI is:
//
//   extern "C" {
//     const bps::PluginManifest* bps_plugin_manifest(void);
//     bps::IPlugin*              bps_plugin_create(void);
//     void                       bps_plugin_destroy(bps::IPlugin*);
//   }
class IPlugin : public IService {
public:
    ~IPlugin() override = default;

    const char* ServiceName() const noexcept override { return Manifest().id.c_str(); }

    virtual const PluginManifest& Manifest() const = 0;

    virtual Result<void> OnLoad()   { return Ok(); }
    virtual Result<void> OnStart()  { return Ok(); }
    virtual Result<void> OnStop()   { return Ok(); }
    virtual Result<void> OnUnload() { return Ok(); }
};

} // namespace bps
