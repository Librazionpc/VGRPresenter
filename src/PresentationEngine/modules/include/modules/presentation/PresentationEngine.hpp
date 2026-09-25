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
#include "modules/presentation/PresentationDocument.hpp"
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
    // Registers (or refreshes) a full presentation copy under its own id and
    // returns that id. The WORKING SHOW path: ShowService edits the
    // PresentationDocument, and the live loop needs the same content in the
    // registry — without this, StartFromOpenShow's Open(id) failed with
    // "presentation not found" and nothing ever went on air. Idempotent: an
    // id that is already registered is overwritten in place (same id, fresh
    // content), so live re-syncs see edits.
    Result<std::string> PutPresentation(const Presentation& presentation);
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

    // --- Live binding (any content on air) ---------------------------------------
    // Binds the RUNTIME to the caller's copy (the caller keeps it alive for the
    // whole live run — the registry mirror churns, so the runtime must never
    // point into it), runs compile -> prepare (under the active output style)
    // -> GoLive. This is the honest fix for go-live: the working show used to
    // be opened by registry id it was never registered under.
    Result<void> PresentLive(const Presentation& content);
    // Swaps the live content in place — no state-machine transitions, legal
    // while LIVE. Rebinds, recompiles and rebuilds every scene under the
    // active output style; playback restarts at the first slide.
    Result<void> SwapLiveContent(const Presentation& content);

    // --- Output style (Settings · Styles applied to the on-air output) -----------
    // The ONE spec the live render loop composes with (empty = unstyled). The
    // UI pushes a style change the moment it's edited; the next frame's 1s
    // re-Prepare rebuilds the affected scenes under the new spec (SceneBuilder
    // keys styled scenes by a fingerprint of the spec, so nothing stale can be
    // served). Thread-safe: the live loop runs on its own thread.
    Result<void> SetActiveOutputStyle(const OutputStyleSpec& style);
    OutputStyleSpec ActiveOutputStyle() const;
    // The scene builder, for the live loop's style rebuilds (RebuildScenes
    // runs on the loop thread; the builder itself is stateless per call).
    SceneBuilder& Builder() const { return *builder_; }

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

public:
    // The working show and its .vgr open/save (docs/specs/22). Null before
    // Initialize() / after Shutdown().
    std::shared_ptr<PresentationDocument> Document() const;

private:

    // Dependency wiring (set at Initialize): the RenderEngine and the builders.
    void WireDependencies();

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Presentation>, std::less<>> registry_;
    std::string activeId_;
    PresentationRuntime runtime_;
    PresentationSession session_;
    PresentationValidator validator_;
    // The on-air output's style (Set/Active below). Guarded by mutex_, read
    // every frame by PresentationEngine::Prepare's builder call path.
    OutputStyleSpec activeStyle_;
    std::shared_ptr<PresentationCompiler> compiler_;
    std::shared_ptr<SceneBuilder> builder_;
    // The "presentation" document handler (open/save the working show as .vgr),
    // registered with the DocumentManager while the engine is initialized.
    std::shared_ptr<PresentationDocument> document_;
    std::vector<Subscription> subscriptions_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> cueCount_{0};
    std::atomic<uint64_t> recoveredCount_{0};
};

} // namespace bps::presentation
