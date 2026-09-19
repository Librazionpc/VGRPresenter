#pragma once

#include "core/common/Common.hpp"
#include "interfaces/IRenderer.hpp"
#include "interfaces/IService.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace bps {

// Renderer registry + frame dispatch (core/rendering/). The active renderer is
// a registered IRenderer (OpenGL/Vulkan/software backends); modules only ever
// see this manager and the IRenderer interface. Frames are timed here and
// reported through Stats() (performance logging, 02 §Performance).
class RendererManager final : public IService {
public:
    static RendererManager& Instance();

    Result<void> Initialize() override;
    Result<void> Shutdown() override;

    Result<void> RegisterRenderer(std::string name, IRenderer* renderer);
    Result<void> SetActive(std::string_view name);
    Result<IRenderer*> Active() const;

    Result<void> BeginFrame();
    Result<void> EndFrame();
    Result<void> Present();   // Present() + frame accounting

    struct FrameStats {
        double fps = 0.0;
        double frameMsP50 = 0.0;
        double frameMsP95 = 0.0;
        uint64_t frames = 0;
    };
    FrameStats Stats() const;
    size_t RendererCount() const;

    const char* ServiceName() const noexcept override { return "RendererManager"; }
    HealthReport GetHealth() const override;

private:
    RendererManager() = default;
    void RecordFrame(std::chrono::microseconds frameTime);

    mutable std::mutex mutex_;
    std::map<std::string, IRenderer*> renderers_;     // guarded by mutex_
    std::string activeName_;                          // guarded by mutex_
    EngineTime frameBegin_;                           // guarded by mutex_
    std::deque<double> frameTimesMs_;                 // guarded by mutex_ (rolling window)
    uint64_t frames_ = 0;                             // guarded by mutex_
    static constexpr size_t kWindow = 120;
    std::atomic<bool> initialized_{false};
};

} // namespace bps
