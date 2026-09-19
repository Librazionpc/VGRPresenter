#pragma once

// PresentationEngine (docs/specs/19): the Phase 8 facade. Manager
// (create/open/save/duplicate/delete/validate/compile/register/close),
// runtime + controller (Next/Previous/JumpTo/GoLive/Black/Logo/Pause/Resume/
// Stop), queue, session + recovery. Orchestrates the live presentation
// workflow without containing rendering or display logic.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/presentation/PresentationCues.hpp"
#include "modules/presentation/PresentationRuntime.hpp"
#include "modules/presentation/PresentationSession.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/PresentationValidator.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bps::rendering {
class RenderEngine;
}
namespace bps::presentation {
class PresentationCompiler;
class SceneBuilder;

class PresentationEngine final : public IService {
public:
    static PresentationEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "PresentationEngine"; }

    // --- Manager ---------------------------------------------------------------
    Result<std::string> CreatePresentation(std::string_view name);
    Result<void> Open(std::string_view id);
    Result<void> Close();
    Result<void> Save();                        // session snapshot
    Result<void> Duplicate(std::string_view id);
    Result<void> Delete(std::string_view id);
    Result<std::shared_ptr<Presentation>> Get(std::string_view id) const;
    std::vector<std::string> PresentationIds() const;
    size_t PresentationCount() const;
    std::string ActivePresentationId() const { return activeId_; }

    // Slide editing on the registry copy (stable ids).
    Result<std::string> AddSlide(std::string_view presentationId, const Slide& slide);
    Result<void> RemoveSlide(std::string_view presentationId, std::string_view slideId);

    // --- Pipeline ----------------------------------------------------------------
    Result<std::vector<ValidationIssue>> Validate(std::string_view id);
    Result<void> Compile(std::string_view id);
    Result<void> Prepare(std::string_view id);   // builds scenes via SceneBuilder

    // --- Controller (active presentation) -----------------------------------------
    Result<void> GoLive();
    Result<void> Pause();
    Result<void> Resume();
    Result<void> StopPlayback();
    Result<void> Next();
    Result<void> Previous();
    Result<void> JumpTo(size_t index);
    Result<void> JumpById(std::string_view slideId);
    Result<size_t> Search(std::string_view query) const;

    // --- Timeline / cues ----------------------------------------------------------
    Result<void> AddCue(std::shared_ptr<IPresentationCue> cue);
    Result<void> ClearTimeline();

    // --- Playback ---------------------------------------------------------------
    // Advance by dt seconds (called by the Kernel's presentation tick).
    Result<size_t> Tick(double dt);
    PresentationState State() const;
    size_t CurrentIndex() const;
    const Slide* CurrentSlide() const;
    PresentationRuntime& Runtime() { return runtime_; }
    const PresentationRuntime& Runtime() const { return runtime_; }

    // --- Session / recovery --------------------------------------------------------
    Result<void> SaveSession();
    Result<SessionSnapshot> Recover();

    // --- Events -------------------------------------------------------------------
    void WireEvents();
    void UnwireEvents();
    void OnConfigReload(const events::ConfigHotReload& e);

private:
    PresentationEngine() = default;

    // Dependency wiring (set at Initialize): the RenderEngine and the builders.
    void WireDependencies();

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Presentation>, std::less<>> registry_;
    std::string activeId_;
    PresentationRuntime runtime_;
    PresentationSession session_;
    PresentationValidator validator_;
    std::shared_ptr<PresentationCompiler> compiler_;
    std::shared_ptr<SceneBuilder> builder_;
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> cueCount_{0};
    std::atomic<uint64_t> recoveredCount_{0};
};

} // namespace bps::presentation
