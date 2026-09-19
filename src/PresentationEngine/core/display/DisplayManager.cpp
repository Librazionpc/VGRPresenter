#include "core/display/DisplayManager.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include <format>

namespace bps {

DisplayManager& DisplayManager::Instance() {
    static DisplayManager instance;
    return instance;
}

Result<void> DisplayManager::Initialize() {
    initialized_.store(true);
    return Ok();
}

Result<void> DisplayManager::Shutdown() {
    if (!initialized_.exchange(false)) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    outputMap_.clear();
    displays_.clear();
    return Ok();
}

Result<void> DisplayManager::RegisterDisplay(std::string name, IDisplay* display) {
    if (name.empty() || !display)
        return Error::Make(Err::InvalidArgument, "DisplayManager",
                           "display name and pointer required");
    auto infos = display->Displays();   // outside the lock: a backend may call back in
    std::lock_guard<std::mutex> lock(mutex_);
    if (displays_.count(name))
        return Error::Make(Err::AlreadyExists, "DisplayManager",
                           "display already registered: " + name);
    displays_[name] = display;
    outputMap_[name] = std::vector<bool>(infos.size(), false);
    return Ok();
}

Result<void> DisplayManager::RemoveDisplay(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!displays_.erase(std::string(name)))
        return Error::Make(Err::NotFound, "DisplayManager",
                           "unknown display: " + std::string(name));
    outputMap_.erase(std::string(name));
    return Ok();
}

std::vector<DisplayInfo> DisplayManager::Enumerate() const {
    std::vector<std::pair<std::string, IDisplay*>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, d] : displays_) copy.emplace_back(name, d);
    }
    std::vector<DisplayInfo> out;
    for (const auto& [name, d] : copy) {
        (void)name;
        auto infos = d->Displays();
        out.insert(out.end(), infos.begin(), infos.end());
    }
    return out;
}

Result<void> DisplayManager::SetOutput(std::string_view displayName, int index, bool enabled) {
    IDisplay* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = displays_.find(std::string(displayName));
        if (it == displays_.end())
            return Error::Make(Err::NotFound, "DisplayManager",
                               "unknown display: " + std::string(displayName));
        auto& map = outputMap_[std::string(displayName)];
        if (index < 0 || static_cast<size_t>(index) >= map.size())
            return Error::Make(Err::InvalidArgument, "DisplayManager",
                               "display index out of range");
        map[static_cast<size_t>(index)] = enabled;
        d = it->second;
    }
    if (auto r = d->SetOutput(index, enabled); !r.ok()) return r;
    (void)EventBus::Instance().Publish(
        events::DisplayChanged{std::string(displayName), index, enabled});
    return Ok();
}

bool DisplayManager::IsOutputEnabled(std::string_view displayName, int index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = outputMap_.find(std::string(displayName));
    if (it == outputMap_.end() || index < 0 || static_cast<size_t>(index) >= it->second.size())
        return false;
    return it->second[static_cast<size_t>(index)];
}

size_t DisplayManager::DisplayCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return displays_.size();
}

HealthReport DisplayManager::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    size_t outputs = 0;
    for (const auto& [name, map] : outputMap_) {
        (void)name;
        outputs += map.size();
    }
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("backends={} outputs={}", displays_.size(), outputs);
    return r;
}

} // namespace bps
