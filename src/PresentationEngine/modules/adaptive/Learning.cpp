#include "modules/adaptive/Learning.hpp"

#include "core/database/DatabaseManager.hpp"

#include <algorithm>
#include <chrono>

namespace bps::adaptive {

namespace {
int TodayWeekday() {
    // 0=Sunday .. 6=Saturday (matches the scheduler's convention).
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm.tm_wday;
}
} // namespace

// ---------------------------------------------------------------------------
// UsageLearningEngine
// ---------------------------------------------------------------------------

void UsageLearningEngine::RecordUsage(std::string_view kind, std::string_view id,
                                      int weekday) {
    std::lock_guard<std::mutex> lock(mutex_);
    Key k{std::string(kind), std::string(id)};
    ++counts_[k];
    ++total_;
    int wd = weekday < 0 ? TodayWeekday() : weekday % 7;
    ++weekdayCounts_[wd][k];
}

double UsageLearningEngine::Frequency(std::string_view kind, std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_ == 0) return 0.0;
    Key k{std::string(kind), std::string(id)};
    auto it = counts_.find(k);
    if (it == counts_.end()) return 0.0;
    return static_cast<double>(it->second) / static_cast<double>(total_);
}

std::vector<std::string> UsageLearningEngine::Top(std::string_view kind, int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<double, std::string>> scored;
    for (const auto& [k, count] : counts_) {
        if (k.kind != kind) continue;
        scored.emplace_back(static_cast<double>(count), k.id);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (size_t i = 0; i < scored.size() && static_cast<int>(out.size()) < limit; ++i)
        out.push_back(scored[i].second);
    return out;
}

Result<void> UsageLearningEngine::Save() {
    json::Value::Array arr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, count] : counts_) {
            json::Value::Object o;
            o["kind"] = json::Value::String(k.kind);
            o["id"] = json::Value::String(k.id);
            o["count"] = json::Value::Number(static_cast<double>(count));
            arr.push_back(json::Value(std::move(o)));
        }
    }
    return DatabaseManager::Instance().Put("adaptive-usage", "counts", json::Value(std::move(arr)));
}

Result<void> UsageLearningEngine::Load() {
    auto doc = DatabaseManager::Instance().Get("adaptive-usage", "counts");
    if (!doc.ok()) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    counts_.clear();
    total_ = 0;
    if (doc.value().type() != json::Value::Type::Array) return Ok();
    for (const auto& e : *doc.value().asArray()) {
        Key k;
        if (const auto* n = e.Find("kind")) k.kind = std::string(n->asString());
        if (const auto* n = e.Find("id")) k.id = std::string(n->asString());
        uint64_t count = 0;
        if (const auto* n = e.Find("count")) count = static_cast<uint64_t>(n->asInt(0));
        if (count > 0) {
            counts_[k] = count;
            total_ += count;
        }
    }
    return Ok();
}

size_t UsageLearningEngine::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_.size();
}

// ---------------------------------------------------------------------------
// SmartCacheManager
// ---------------------------------------------------------------------------

bool SmartCacheManager::ShouldPreload(std::string_view kind, std::string_view id) const {
    if (!preloadEnabled_.load()) return false;
    if (!learning_) return false;
    // Preload items the user has opened more than once (frequency ≥ 2 uses).
    return learning_->Frequency(kind, id) >= 2.0 / 100.0;
}

std::vector<std::string> SmartCacheManager::PreloadCandidates(std::string_view kind,
                                                              int limit) const {
    if (!preloadEnabled_.load() || !learning_) return {};
    return learning_->Top(kind, limit);
}

// ---------------------------------------------------------------------------
// StartupOptimizer
// ---------------------------------------------------------------------------

std::vector<std::string> StartupOptimizer::LearnedSet(double minFrequency) const {
    std::vector<std::string> out;
    if (!learning_) return out;
    // "module" kind tracks which feature modules the user actually opens.
    for (const auto& id : learning_->Top("module", 64)) {
        if (learning_->Frequency("module", id) >= minFrequency) out.push_back(id);
    }
    return out;
}

std::vector<std::string> StartupOptimizer::RecommendedModules(double minFrequency) const {
    auto core = CoreSet();
    auto learned = LearnedSet(minFrequency);
    core.insert(core.end(), learned.begin(), learned.end());
    return core;
}

// ---------------------------------------------------------------------------
// RecommendationEngine
// ---------------------------------------------------------------------------

std::vector<Recommendation> RecommendationEngine::Recommend(bool gpuAvailable,
                                                            QualityLevel currentLevel,
                                                            uint64_t totalRamBytes,
                                                            bool onBattery) const {
    std::vector<Recommendation> out;
    if (gpuAvailable && currentLevel < QualityLevel::High)
        out.push_back({"Your PC supports GPU rendering — enable higher quality?",
                       "enable-gpu"});
    if (totalRamBytes > 0 && totalRamBytes < (4ull << 30) && currentLevel > QualityLevel::Performance)
        out.push_back({"Detected under 4 GB RAM — the Performance profile is recommended.",
                       "switch-performance"});
    if (onBattery)
        out.push_back({"On battery power — Battery Saver profile is recommended.",
                       "switch-battery"});
    return out;
}

} // namespace bps::adaptive
