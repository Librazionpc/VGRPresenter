#pragma once

#include "core/common/Common.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"
#include "interfaces/IModule.hpp"

#include <atomic>

namespace bps::modules {

// Example feature module (docs/specs/08): listens for the canonical
// `presentation.slide_changed` event and fans it out via the Logger, and runs
// a real autoplay feature (07 §Autoplay) that advances slides on the
// TaskScheduler. Demonstrates how modules plug into the core without the core
// knowing them.
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
