#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IDisplay.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps {

// Display/output registry (core/display/). Manages IDisplay backends and the
// current output map. Which physical outputs are live is decided here; actual
// pixel work is the RendererManager's job.
class DisplayManager final : public IService {
public:
    static DisplayManager& Instance();

    Result<void> Initialize() override;
    Result<void> Shutdown() override;

    Result<void> RegisterDisplay(std::string name, IDisplay* display);
    Result<void> RemoveDisplay(std::string_view name);
    std::vector<DisplayInfo> Enumerate() const;                  // union across backends
    Result<void> SetOutput(std::string_view displayName, int index, bool enabled);
    bool IsOutputEnabled(std::string_view displayName, int index) const;
    size_t DisplayCount() const;

    const char* ServiceName() const noexcept override { return "DisplayManager"; }
    HealthReport GetHealth() const override;

private:
    DisplayManager() = default;

    mutable std::mutex mutex_;
    std::map<std::string, IDisplay*> displays_;               // guarded by mutex_
    std::map<std::string, std::vector<bool>> outputMap_;      // guarded by mutex_
    std::atomic<bool> initialized_{false};
};

} // namespace bps
