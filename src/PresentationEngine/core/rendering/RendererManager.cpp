#include "core/rendering/RendererManager.hpp"

#include <algorithm>
#include <format>

namespace bps {

RendererManager& RendererManager::Instance() {
    static RendererManager instance;
    return instance;
}

Result<void> RendererManager::Initialize() {
    initialized_.store(true);
    return Ok();
}

Result<void> RendererManager::Shutdown() {
    if (!initialized_.exchange(false)) return Ok();
    std::lock_guard<std::mutex> lock(mutex_);
    renderers_.clear();
    activeName_.clear();
    frameTimesMs_.clear();
    frames_ = 0;
    return Ok();
}

Result<void> RendererManager::RegisterRenderer(std::string name, IRenderer* renderer) {
    if (name.empty() || !renderer)
        return Error::Make(Err::InvalidArgument, "RendererManager",
                           "renderer name and pointer required");
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderers_.count(name))
        return Error::Make(Err::AlreadyExists, "RendererManager",
                           "renderer already registered: " + name);
    renderers_[name] = renderer;
    if (activeName_.empty()) activeName_ = name;   // first registered wins
    return Ok();
}

Result<void> RendererManager::SetActive(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!renderers_.count(std::string(name)))
        return Error::Make(Err::NotFound, "RendererManager",
                           "unknown renderer: " + std::string(name));
    activeName_ = std::string(name);
    return Ok();
}

Result<IRenderer*> RendererManager::Active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = renderers_.find(activeName_);
    if (it == renderers_.end())
        return Error::Make(Err::NotFound, "RendererManager", "no active renderer");
    return it->second;
}

Result<void> RendererManager::BeginFrame() {
    IRenderer* r = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = renderers_.find(activeName_);
        if (it == renderers_.end())
            return Error::Make(Err::NotFound, "RendererManager", "no active renderer");
        r = it->second;
        frameBegin_ = EngineClock::now();
    }
    return r->BeginFrame();
}

Result<void> RendererManager::EndFrame() {
    IRenderer* r = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = renderers_.find(activeName_);
        if (it == renderers_.end())
            return Error::Make(Err::NotFound, "RendererManager", "no active renderer");
        r = it->second;
    }
    return r->EndFrame();
}

Result<void> RendererManager::Present() {
    IRenderer* r = nullptr;
    EngineTime begin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = renderers_.find(activeName_);
        if (it == renderers_.end())
            return Error::Make(Err::NotFound, "RendererManager", "no active renderer");
        r = it->second;
        begin = frameBegin_;   // copy under the lock (BeginFrame wrote it under the lock)
    }
    if (auto res = r->Present(); !res.ok()) return res;
    RecordFrame(std::chrono::duration_cast<std::chrono::microseconds>(EngineClock::now() -
                                                                      begin));
    return Ok();
}

void RendererManager::RecordFrame(std::chrono::microseconds frameTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;
    frameTimesMs_.push_back(std::chrono::duration<double, std::milli>(frameTime).count());
    while (frameTimesMs_.size() > kWindow) frameTimesMs_.pop_front();
}

RendererManager::FrameStats RendererManager::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FrameStats s;
    s.frames = frames_;
    if (frameTimesMs_.empty()) return s;
    std::vector<double> v(frameTimesMs_.begin(), frameTimesMs_.end());
    std::sort(v.begin(), v.end());
    s.frameMsP50 = v[v.size() / 2];
    size_t idx95 = static_cast<size_t>(v.size() * 0.95);
    if (idx95 == 0) idx95 = 1;
    s.frameMsP95 = v[idx95 - 1];
    double sum = 0;
    for (double x : v) sum += x;
    double avg = sum / static_cast<double>(v.size());
    s.fps = avg > 0.0 ? 1000.0 / avg : 0.0;
    return s;
}

size_t RendererManager::RendererCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return renderers_.size();
}

HealthReport RendererManager::GetHealth() const {
    HealthReport r;
    std::lock_guard<std::mutex> lock(mutex_);
    r.state = initialized_.load() ? HealthState::Healthy : HealthState::Degraded;
    r.detail = std::format("renderers={}{}", renderers_.size(),
                           activeName_.empty() ? "" : " active=" + activeName_);
    return r;
}

} // namespace bps
