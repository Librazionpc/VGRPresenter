#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IDriver.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bps {

// Hardware driver registry (core/drivers/). The core owns the registry;
// feature modules provide IDriver implementations (NDI, capture, MIDI, ...) and
// resolve them here instead of constructing hardware directly. Drivers are
// thin adapters: Probe / Enable / Disable only.
class DriverManager final : public IService {
public:
    static DriverManager& Instance();

    Result<void> Initialize() override;
    Result<void> Shutdown() override;

    Result<void> Register(std::string name, IDriver* driver);
    Result<void> Unregister(std::string_view name);
    Result<IDriver*> Resolve(std::string_view name) const;
    Result<void> ProbeAll();
    Result<void> EnableAll();
    Result<void> DisableAll();
    using IService::Reload;
    Result<void> Reload(std::string_view name);   // disable + enable (00 §Hot Reload)

    std::vector<std::pair<std::string, bool>> Snapshot() const;   // name, enabled

    const char* ServiceName() const noexcept override { return "DriverManager"; }
    HealthReport GetHealth() const override;

private:
    DriverManager() = default;

    mutable std::mutex mutex_;
    std::map<std::string, IDriver*> drivers_;         // guarded by mutex_
    std::map<std::string, bool> enabled_;             // guarded by mutex_
    std::atomic<bool> initialized_{false};
};

} // namespace bps
