#pragma once

#include "core/common/Common.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "interfaces/IModule.hpp"

#include <atomic>

namespace bps::modules {
class PresentationModule final : public IModule {
public:
    static std::shared_ptr<PresentationModule> Create();

    const ModuleManifest& Manifest() const override;

    Result<void> OnLoad() override;
    Result<void> OnUnload() override;
    Result<void> OnStart() override;
    Result<void> OnStop() override;
    Result<void> OnPause() override;
    Result<void> OnResume() override;

    // Autoplay (07 §Autoplay): advances through `slideCount` slides, publishing
    // `presentation.slide_changed` every `interval`. Pause/Stop via the handle.
    Result<void> StartAutoplay(size_t slideCount, std::chrono::milliseconds interval);
    Result<void> StopAutoplay();
    bool AutoplayActive() const noexcept { return autoplayTask_ != 0; }
    int AutoplayPosition() const noexcept { return autoplayPosition_.load(); }

private:
    PresentationModule() = default;
    Subscription sub_;
    bool started_ = false;
    TaskId autoplayTask_ = 0;
    size_t autoplaySlides_ = 0;
    std::atomic<int> autoplayPosition_{0};
};
} // namespace bps::modules