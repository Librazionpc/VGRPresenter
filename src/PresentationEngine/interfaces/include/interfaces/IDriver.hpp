#pragma once

#include "interfaces/IService.hpp"

namespace bps {

// Hardware driver interface (docs/specs/10, core/drivers/).
// Drivers are thin adapters over physical devices (NDI, capture, MIDI, displays).
class IDriver : public IService {
public:
    ~IDriver() override = default;

    virtual Result<void> Probe()   = 0;   // enumerate/detect the device
    virtual Result<void> Enable()  = 0;   // open + start
    virtual Result<void> Disable() = 0;   // stop + close
};

} // namespace bps
