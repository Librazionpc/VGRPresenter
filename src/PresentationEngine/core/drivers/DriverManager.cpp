#include "core/drivers/DriverManager.hpp"

#include <vector>
#include <format>

namespace bps {

DriverManager& DriverManager::Instance() {
    static DriverManager instance;
    return instance;
}

Result<void> DriverManager::Initialize() {
    initialized_.store(true);
    return Ok();
}

Result<void> DriverManager::Shutdown() {
    if (!initialized_.exchange(false)) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_.clear();
    drivers_.clear();
    return Ok();
}

Result<void> DriverManager::Register(std::string name, IDriver* driver) {
    if (name.empty() || !driver)
        return Error::Make(Err::InvalidArgument, "DriverManager",
                           "driver name and pointer required");
    std::lock_guard<std::mutex> lock(mutex_);
    if (drivers_.count(name))
        return Error::Make(Err::AlreadyExists, "DriverManager",
                           "driver already registered: " + name);
    drivers_[name] = driver;
    enabled_[name] = false;
    return Ok();
}

Result<void> DriverManager::Unregister(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!drivers_.erase(std::string(name)))
        return Error::Make(Err::NotFound, "DriverManager",
                           "unknown driver: " + std::string(name));
    enabled_.erase(std::string(name));
    return Ok();
}

Result<IDriver*> DriverManager::Resolve(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = drivers_.find(std::string(name));
    if (it == drivers_.end())
        return Error::Make(Err::NotFound, "DriverManager",
                           "unknown driver: " + std::string(name));
    return it->second;
}

Result<void> DriverManager::ProbeAll() {
    std::vector<IDriver*> drivers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, d] : drivers_) {
            (void)name;
            drivers.push_back(d);
        }
    }
    for (IDriver* d : drivers)
        if (auto r = d->Probe(); !r.ok()) return r;
    return Ok();
}

Result<void> DriverManager::EnableAll() {
    std::vector<std::pair<std::string, IDriver*>> drivers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, d] : drivers_) drivers.emplace_back(name, d);
    }
    for (const auto& [name, d] : drivers) {
        if (auto r = d->Enable(); !r.ok()) return r;
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_[name] = true;
    }
    return Ok();
}

Result<void> DriverManager::DisableAll() {
    std::vector<std::pair<std::string, IDriver*>> drivers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, d] : drivers_) drivers.emplace_back(name, d);
    }
    for (const auto& [name, d] : drivers) {
        if (auto r = d->Disable(); !r.ok()) return r;
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_[name] = false;
    }
    return Ok();
}

Result<void> DriverManager::Reload(std::string_view name) {
    IDriver* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = drivers_.find(std::string(name));
        if (it == drivers_.end())
            return Error::Make(Err::NotFound, "DriverManager",
                               "unknown driver: " + std::string(name));
        d = it->second;
    }
    if (auto r = d->Disable(); !r.ok()) return r;
    if (auto r = d->Enable(); !r.ok()) return r;
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_[std::string(name)] = true;
    return Ok();
}

std::vector<std::pair<std::string, bool>> DriverManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, bool>> out;
    out.reserve(drivers_.size());
    for (const auto& [name, d] : drivers_) {
        (void)d;
        out.emplace_back(name, enabled_.at(name));
    }
    return out;
}

HealthReport DriverManager::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("drivers={}", drivers_.size());
    return r;
}

} // namespace bps
