#pragma once

#include "interfaces/IService.hpp"

namespace bps {

// Feature-package manifest (00 §6 Versioning). Every module and plugin carries one.
struct ModuleManifest {
    std::string id;                          // e.g. "presentation"
    Version version;
    std::string author;
    std::vector<std::string> dependencies;   // module/plugin ids
    Version requiredCoreVersion;             // min engine core version
    std::vector<std::string> capabilities;   // machine-readable tags
};

// Module interface (docs/specs/08). Modules are the feature packages of the
// engine (Presentation, Bible, Media, Songs, Streaming, NDI, MIDI, AI, Cloud,
// Remote). All lifecycle entry points are optional — default to Ok().
class IModule : public IService {
public:
    ~IModule() override = default;

    const char* ServiceName() const noexcept override { return Manifest().id.c_str(); }

    virtual const ModuleManifest& Manifest() const = 0;

    virtual Result<void> OnLoad()   { return Ok(); }
    virtual Result<void> OnStart()  { return Ok(); }
    virtual Result<void> OnStop()   { return Ok(); }
    virtual Result<void> OnUnload() { return Ok(); }
    virtual Result<void> OnPause()  { return Ok(); }
    virtual Result<void> OnResume() { return Ok(); }
    virtual Result<void> OnSuspend(){ return Ok(); }
};

} // namespace bps
