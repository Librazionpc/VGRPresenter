#include "modules/adaptive/Contracts.hpp"

#include <algorithm>

namespace bps::adaptive {

Result<void> FeatureManager::Register(const FeatureDef& def) {
    if (def.id.empty())
        return Error::Make(Err::InvalidArgument, "Adaptive", "feature id must not be empty");
    std::lock_guard<std::mutex> lock(mutex_);
    if (features_.count(def.id))
        return Error::Make(Err::AlreadyExists, "Adaptive",
                           "feature '" + def.id + "' already registered");
    features_[def.id] = def;
    return Ok();
}

Result<void> FeatureManager::Unregister(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!features_.erase(std::string(id)))
        return Error::Make(Err::NotFound, "Adaptive", "no feature '" + std::string(id) + "'");
    return Ok();
}

Result<void> FeatureManager::SetEnabled(std::string_view id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = features_.find(id);
    if (it == features_.end())
        return Error::Make(Err::NotFound, "Adaptive", "no feature '" + std::string(id) + "'");
    if (enabled)
        it->second.state = it->second.state == FeatureState::Suspended
                               ? FeatureState::Suspended   // explicit suspend wins
                               : FeatureState::Enabled;
    else
        it->second.state = FeatureState::Disabled;
    return Ok();
}

Result<void> FeatureManager::SetSuspended(std::string_view id, bool suspended) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = features_.find(id);
    if (it == features_.end())
        return Error::Make(Err::NotFound, "Adaptive", "no feature '" + std::string(id) + "'");
    if (suspended) {
        if (it->second.state != FeatureState::Disabled)
            it->second.state = FeatureState::Suspended;
    } else if (it->second.state == FeatureState::Suspended) {
        it->second.state = FeatureState::Enabled;
    }
    return Ok();
}

Result<void> FeatureManager::SetThrottled(std::string_view id, bool throttled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = features_.find(id);
    if (it == features_.end())
        return Error::Make(Err::NotFound, "Adaptive", "no feature '" + std::string(id) + "'");
    if (throttled) {
        if (it->second.state == FeatureState::Enabled) it->second.state = FeatureState::Throttled;
    } else if (it->second.state == FeatureState::Throttled) {
        it->second.state = FeatureState::Enabled;
    }
    return Ok();
}

Result<FeatureState> FeatureManager::State(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = features_.find(id);
    if (it == features_.end())
        return Error::Make(Err::NotFound, "Adaptive", "no feature '" + std::string(id) + "'");
    return Result<FeatureState>{it->second.state};
}

bool FeatureManager::IsEnabled(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = features_.find(id);
    return it != features_.end() && it->second.state != FeatureState::Disabled;
}

std::optional<ModuleContract> FeatureManager::Contract(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = features_.find(id);
    if (it == features_.end()) return std::nullopt;
    return it->second.contract;
}

std::vector<FeatureDef> FeatureManager::All() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FeatureDef> out;
    out.reserve(features_.size());
    for (const auto& [id, def] : features_) {
        (void)id;
        out.push_back(def);
    }
    return out;
}

size_t FeatureManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return features_.size();
}

FeatureState FeatureManager::RecommendState(const FeatureDef& def, uint64_t totalRamBytes,
                                            bool gpuAvailable) const {
    if (!def.enabledByDefault) return FeatureState::Disabled;
    if (def.contract.gpuRequired && !gpuAvailable) return FeatureState::Disabled;
    if (totalRamBytes > 0 && def.contract.minRamBytes > 0 &&
        totalRamBytes < def.contract.minRamBytes)
        return FeatureState::Disabled;
    if (def.contract.minRamBytes > 0 && totalRamBytes < def.contract.recommendedRamBytes)
        return FeatureState::Throttled;
    return FeatureState::Enabled;
}

} // namespace bps::adaptive
