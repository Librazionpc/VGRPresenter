#include "modules/presentation/PresentationModule.hpp"

#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "core/logging/Logger.hpp"
#include "core/task_scheduler/TaskScheduler.hpp"

#include <format>

namespace bps::modules {

std::shared_ptr<PresentationModule> PresentationModule::Create() {
    return std::shared_ptr<PresentationModule>(new PresentationModule());
}

const ModuleManifest& PresentationModule::Manifest() const {
    static ModuleManifest m;
    m.id = "presentation";
    m.version = kEngineVersion;
    m.author = "BPS Core";
    m.requiredCoreVersion = kEngineVersion;
    m.capabilities = {"slides", "playlist", "stages"};
    return m;
}

Result<void> PresentationModule::OnLoad() {
    sub_ = EventBus::Instance().Subscribe<events::SlideChanged>(
        [](const events::SlideChanged& e) {
            // Canonical fan-out example (05 §7): Display/Remote/NDI/OBS would each
            // subscribe independently — here the logger stands in for them.
            Logger::Instance().Info(std::format("SlideChanged -> display/remote/ndi/obs: {}/{} \"{}\"",
                                                e.index + 1, e.total, e.title),
                                    "Presentation");
        },
        0);
    return Ok();
}

Result<void> PresentationModule::OnUnload() {
    (void)StopAutoplay();
    if (sub_.Valid()) (void)EventBus::Instance().Unsubscribe(sub_);
    sub_ = Subscription{};
    return Ok();
}

Result<void> PresentationModule::StartAutoplay(size_t slideCount,
                                               std::chrono::milliseconds interval) {
    if (!started_) return Error::Make(Err::InvalidState, "PresentationModule", "module not started");
    (void)StopAutoplay();
    auto res = TaskScheduler::Instance().ScheduleSequence(
        [this](size_t i) {
            autoplayPosition_.store(static_cast<int>(i + 1));
            (void)EventBus::Instance().Publish(events::SlideChanged{
                static_cast<int>(i), static_cast<int>(autoplaySlides_),
                std::format("Slide {}", i + 1)});
        },
        slideCount, std::chrono::duration_cast<std::chrono::microseconds>(interval));
    if (!res.ok()) return res.error();
    autoplayTask_ = res.value();
    autoplaySlides_ = slideCount;
    autoplayPosition_.store(0);
    Logger::Instance().Info(std::format("Autoplay started: {} slides @ {}ms", slideCount,
                                        interval.count()),
                            "Presentation");
    return Ok();
}

Result<void> PresentationModule::StopAutoplay() {
    if (autoplayTask_ != 0) {
        (void)TaskScheduler::Instance().Cancel(autoplayTask_);
        autoplayTask_ = 0;
    }
    return Ok();
}

Result<void> PresentationModule::OnStart() {
    started_ = true;
    Logger::Instance().Info("Presentation module started", "Presentation");
    return Ok();
}

Result<void> PresentationModule::OnStop() {
    started_ = false;
    Logger::Instance().Info("Presentation module stopped", "Presentation");
    return Ok();
}

Result<void> PresentationModule::OnPause() {
    Logger::Instance().Info("Presentation module paused", "Presentation");
    return Ok();
}

Result<void> PresentationModule::OnResume() {
    Logger::Instance().Info("Presentation module resumed", "Presentation");
    return Ok();
}

} // namespace bps::modules
