#pragma once

#include "interfaces/IService.hpp"
#include <vector>

namespace bps {

struct DisplayInfo {
    int index = 0;
    int width = 0;
    int height = 0;
    int refreshRateHz = 0;
    bool isPrimary = false;
};

// Display/output interface (docs/specs/10 DisplayRegistry, core/display/).
class IDisplay : public IService {
public:
    ~IDisplay() override = default;

    virtual std::vector<DisplayInfo> Displays() const = 0;
    virtual Result<void> SetOutput(int index, bool enabled) = 0;
};

} // namespace bps
