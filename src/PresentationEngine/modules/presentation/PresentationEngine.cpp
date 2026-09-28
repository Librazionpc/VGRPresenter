#include "modules/presentation/PresentationEngine.hpp"

#include "core/logging/Logger.hpp"
#include "modules/presentation/PresentationCompiler.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/project/DocumentManager.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include <chrono>
#include <format>
#include <utility>

namespace bps::presentation {

PresentationEngine& PresentationEngine::Instance() {
    static PresentationEngine instance;
    return instance;
}

void PresentationEngine::WireDependencies() {
    if (!compiler_) compiler_ = std::make_shared<PresentationCompiler>();
    if (!builder_) builder_ = std::make_shared<SceneBuilder>();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Result<void> PresentationEngine::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);
    (void)session_.Initialize();
    WireDependencies();
    WireEvents();
    // Document type "presentation": DocumentManager -> handler -> .vgr file.
    document_ = std::make_shared<PresentationDocument>();
    (void)project::DocumentManager::Instance().RegisterHandler(document_);
    return Ok();
}

std::shared_ptr<PresentationDocument> PresentationEngine::Document() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return document_;
}

Result<void> PresentationEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> PresentationEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> PresentationEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    (void)runtime_.Close();
    UnwireEvents();
    (void)project::DocumentManager::Instance().UnregisterHandler(PresentationDocument::kType);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_.clear();
        activeId_.clear();
        document_.reset();
        initialized_.store(false);
    }
    (void)session_.Shutdown();
    return Ok();
}

Result<void> PresentationEngine::Reload() {
    // Re-validate + re-compile the active presentation.
    if (!activeId_.empty()) {
        auto issues = Validate(activeId_);
        if (issues.ok() && !PresentationValidator::HasErrors(issues.value()))
            (void)Compile(activeId_);
    }
    return Ok();
}

Result<void> PresentationEngine::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    registry_.clear();
    activeId_.clear();
    return Ok();
}

HealthReport PresentationEngine::GetHealth() const {
    HealthReport r;
    r.state = HealthState::Healthy;
    r.detail = std::format("presentations={} active={} state={}", PresentationCount(),
                           activeId_, ToString(runtime_.State()));
    return r;
}

Metrics PresentationEngine::MetricsSnapshot() const {
    Metrics m;
    m.queueLength = cueCount_.load();
    m.errorCount = errorCount_.load();
    m.threadCount = 0;
    m.health = HealthState::Healthy;
    return m;
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------
Result<std::string> PresentationEngine::CreatePresentation(std::string_view name) {
    WireDependencies();
    auto pres = std::make_shared<Presentation>();
    pres->id = std::format("pres-{}", registry_.size() + 1);
    pres->name = std::string(name);
    pres->createdAt = std::chrono::system_clock::now();
    pres->modifiedAt = pres->createdAt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_[pres->id] = pres;
    }
    Logger::Instance().Info("Presentation created: '" + pres->name + "' (" + pres->id + ")",
                            "PresentationEngine");
    return pres->id;
}

Result<std::string> PresentationEngine::PutPresentation(const Presentation& presentation) {
    WireDependencies();
    auto copy = std::make_shared<Presentation>(presentation);
    if (copy->id.empty())
        return Error::Make(Err::Presentation_InvalidState, "PresentationEngine",
                           "presentation has no id");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_[copy->id] = copy;   // insert OR refresh in place
    }
    Logger::Instance().Debug(
        std::format("presentation put: '{}' ({}, {} slides)", copy->name, copy->id,
                    copy->slides.size()),
        "PresentationEngine");
    return copy->id;
}

Result<void> PresentationEngine::Open(std::string_view id) {
    std::shared_ptr<Presentation> pres;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end())
            return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                               "presentation '" + std::string(id) + "' not found");
        pres = it->second;
    }
    auto r = runtime_.Open(*pres);
    if (!r.ok()) return r;
    activeId_ = std::string(id);
    (void)EventBus::Instance().Publish(events::PresentationOpened{
        pres->id, pres->name, pres->slides.size()});
    return Ok();
}

Result<void> PresentationEngine::Close() {
    (void)runtime_.Close();
    if (!activeId_.empty()) {
        (void)EventBus::Instance().Publish(events::PresentationClosed{activeId_});
        activeId_.clear();
    }
    return Ok();
}

Result<void> PresentationEngine::Save() {
    if (activeId_.empty())
        return Error::Make(Err::Presentation_NotOpen, "PresentationEngine",
                           "no active presentation");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(activeId_);
        if (it != registry_.end()) it->second->modifiedAt = std::chrono::system_clock::now();
    }
    return SaveSession();
}

Result<void> PresentationEngine::Duplicate(std::string_view id) {
    std::shared_ptr<Presentation> src;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(id);
        if (it == registry_.end())
            return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                               "presentation '" + std::string(id) + "' not found");
        src = it->second;
    }
    auto copy = std::make_shared<Presentation>(*src);
    copy->id = std::format("pres-{}", registry_.size() + 1);
    copy->name = src->name + " (copy)";
    copy->createdAt = std::chrono::system_clock::now();
    copy->modifiedAt = copy->createdAt;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_[copy->id] = copy;
    }
    return Ok();
}

Result<void> PresentationEngine::Delete(std::string_view id) {
    if (activeId_ == id) (void)Close();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(id);
    if (it == registry_.end())
        return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                           "presentation '" + std::string(id) + "' not found");
    registry_.erase(it);
    return Ok();
}

Result<std::shared_ptr<Presentation>> PresentationEngine::Get(std::string_view id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = registry_.find(id);
    if (it == registry_.end())
        return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                           "presentation '" + std::string(id) + "' not found");
    return it->second;
}

std::vector<std::string> PresentationEngine::PresentationIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(registry_.size());
    for (const auto& [id, _] : registry_) ids.push_back(id);
    return ids;
}

size_t PresentationEngine::PresentationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.size();
}

Result<std::string> PresentationEngine::AddSlide(std::string_view presentationId,
                                                 const Slide& slide) {
    std::shared_ptr<Presentation> pres;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(presentationId);
        if (it == registry_.end())
            return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                               "presentation '" + std::string(presentationId) + "' not found");
        pres = it->second;
    }
    Slide s = slide;
    if (s.id.empty()) s.id = std::format("slide-{}", pres->slides.size() + 1);
    pres->slides.push_back(std::move(s));
    pres->modifiedAt = std::chrono::system_clock::now();
    return pres->slides.back().id;
}

Result<void> PresentationEngine::RemoveSlide(std::string_view presentationId,
                                             std::string_view slideId) {
    std::shared_ptr<Presentation> pres;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(presentationId);
        if (it == registry_.end())
            return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                               "presentation '" + std::string(presentationId) + "' not found");
        pres = it->second;
    }
    for (auto it = pres->slides.begin(); it != pres->slides.end(); ++it) {
        if (it->id == slideId) {
            pres->slides.erase(it);
            return Ok();
        }
    }
    return Error::Make(Err::Presentation_NotFound, "PresentationEngine",
                       "slide '" + std::string(slideId) + "' not found");
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
Result<std::vector<ValidationIssue>> PresentationEngine::Validate(std::string_view id) {
    auto pres = Get(id);
    if (!pres.ok()) return pres.error();
    // Re-open if needed so validation runs against the live runtime.
    if (activeId_ != id) {
        auto r = Open(id);
        if (!r.ok()) return r.error();
    }
    auto issues = runtime_.Validate(validator_);
    if (issues.ok()) {
        size_t w = 0, e = 0;
        for (const auto& i : issues.value()) (i.severity >= 1 ? e : w)++;
        (void)EventBus::Instance().Publish(events::PresentationValidated{
            std::string(id), w, e});
    }
    return issues;
}

Result<void> PresentationEngine::Compile(std::string_view id) {
    auto pres = Get(id);
    if (!pres.ok()) return pres.error();
    if (activeId_ != id) {
        auto r = Open(id);
        if (!r.ok()) return r.error();
    }
    auto r = runtime_.Compile(*compiler_);
    if (r.ok()) {
        (void)EventBus::Instance().Publish(events::PresentationCompiled{
            std::string(id), runtime_.Compiled().slides.size(),
            runtime_.Compiled().warnings});
    }
    return r;
}

Result<void> PresentationEngine::Prepare(std::string_view id) {
    auto pres = Get(id);
    if (!pres.ok()) return pres.error();
    if (activeId_ != id) {
        auto r = Open(id);
        if (!r.ok()) return r.error();
    }
    // The on-air output's style rides into every scene build — SceneBuilder
    // keys styled scenes by a fingerprint of the spec, so a style change made
    // while live is picked up by this 1s re-Prepare without any invalidation
    // pass (stale styled scenes simply stop being asked for).
    OutputStyleSpec style;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        style = activeStyle_;
    }
    return runtime_.Prepare(*builder_, rendering::RenderEngine::Instance(), style);
}

// ---------------------------------------------------------------------------
// Live binding
// ---------------------------------------------------------------------------
Result<void> PresentationEngine::PresentLive(const Presentation& content) {
    if (content.slides.empty())
        return Error::Make(Err::Presentation_NoSlides, "PresentationEngine",
                           "nothing to put on air");

    OutputStyleSpec style;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        style = activeStyle_;
    }

    // The runtime holds a RAW pointer — the caller's copy must outlive the run
    // (the controller keeps its own; the registry mirror below is refreshed
    // independently and never pointed at). A previous binding (any state — a
    // prepared show, a finished run) is closed first: Open refuses while
    // anything is bound.
    (void)runtime_.Close();
    auto open = runtime_.Open(content);
    if (!open.ok()) return open.error();
    activeId_ = content.id;
    // Validate first: Compile checks lastIssues_ (this content's, not a stale
    // set from whatever ran before). Only hard errors abort go-live.
    auto issues = runtime_.Validate(validator_);
    if (!issues.ok()) return issues.error();
    if (PresentationValidator::HasErrors(issues.value()))
        return Error::Make(Err::Presentation_ValidationFailed, "PresentationEngine",
                           "content has validation errors; cannot go live");
    auto compiled = runtime_.Compile(*compiler_);
    if (!compiled.ok()) return compiled.error();
    auto prepared = runtime_.Prepare(*builder_, rendering::RenderEngine::Instance(), style);
    if (!prepared.ok()) return prepared.error();
    auto live = runtime_.GoLive();
    if (!live.ok()) return live.error();
    return Ok();
}

Result<void> PresentationEngine::SwapLiveContent(const Presentation& content) {
    if (content.slides.empty())
        return Error::Make(Err::Presentation_NoSlides, "PresentationEngine",
                           "nothing to put on air");

    OutputStyleSpec style;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        style = activeStyle_;
    }
    // Live -> Live replace: rebind raw pointer + navigator + recompile, then
    // rebuild every scene — the state machine never leaves Live.
    auto swapped = runtime_.SwapLive(content, *compiler_);
    if (!swapped.ok()) return swapped.error();
    activeId_ = content.id;
    return runtime_.RebuildScenes(*builder_, rendering::RenderEngine::Instance(), style);
}

// ---------------------------------------------------------------------------
// Output style (Settings · Styles applied to the on-air output)
// ---------------------------------------------------------------------------
Result<void> PresentationEngine::SetActiveOutputStyle(const OutputStyleSpec& style) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = activeStyle_.name != style.name
            || activeStyle_.contentType != style.contentType
            || activeStyle_.templateKey != style.templateKey
            || activeStyle_.backgroundColor != style.backgroundColor
            || activeStyle_.backgroundImage != style.backgroundImage
            || activeStyle_.clearBackgroundOnText != style.clearBackgroundOnText
            // The BAKED template blocks: an edit to the template design the
            // style wears re-pushes the same key with different blocks — the
            // comparison above alone would call that push a no-op and the
            // output would keep rendering the old layout forever.
            || activeStyle_.showShows != style.showShows
            || activeStyle_.showMedia != style.showMedia
            || activeStyle_.showScripture != style.showScripture
            || activeStyle_.showTable != style.showTable
            || activeStyle_.templateBlocks != style.templateBlocks
            // PER-FAMILY templates: a family re-pick (or a template design
            // edit under a family key) re-pushes with different per-family
            // keys/blocks — that must rebuild scenes, not no-op.
            || !std::equal(std::begin(activeStyle_.familyTemplateKeys), std::end(activeStyle_.familyTemplateKeys),
                           std::begin(style.familyTemplateKeys))
            || !std::equal(std::begin(activeStyle_.familyTemplateBlocks), std::end(activeStyle_.familyTemplateBlocks),
                           std::begin(style.familyTemplateBlocks));
        activeStyle_ = style;
    }
    if (!changed)
        return Ok();   // a no-op push must not thrash the live scenes

    // Wake the live loop: the revision bump is its signal to rebuild every
    // compiled slide's scene under the new spec (RebuildScenes runs from the
    // loop thread, legal while LIVE — no state bounce).
    runtime_.AdvanceStyleRevision();
    Logger::Instance().Info(
        std::format("output style set: '{}' (bg '{}', layout '{}')",
                    style.name, style.backgroundColor, style.templateKey),
        "PresentationEngine");
    return Ok();
}

OutputStyleSpec PresentationEngine::ActiveOutputStyle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeStyle_;
}

Result<void> PresentationEngine::SetLiveOutputStyles(const std::vector<OutputStyleSpec>& styles,
                                                     const std::vector<std::string>& bufferNames) {
    if (styles.size() != bufferNames.size())
        return Error::Make(Err::InvalidArgument, "PresentationEngine",
                           "live style/buffer count mismatch");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        liveOutputStyles_ = styles;
        liveOutputBuffers_ = bufferNames;
    }
    // The live loop re-reads the set every frame and re-keys its passes on a
    // hash of the set — a push lands within a frame, no revision bump needed
    // (the main deck's scenes are unaffected).
    return Ok();
}

std::vector<OutputStyleSpec> PresentationEngine::LiveOutputStyles(
    std::vector<std::string> *outBuffers) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outBuffers)
        *outBuffers = liveOutputBuffers_;
    return liveOutputStyles_;
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------
Result<void> PresentationEngine::GoLive() {
    // Robust entry: if the pipeline hasn't run yet, run it (Loaded -> Validated
    // -> Compiled -> Prepared -> Ready), then go Live. Validation errors abort.
    PresentationState st = runtime_.State();
    if (st == PresentationState::Created || st == PresentationState::Loaded) {
        auto issues = runtime_.Validate(validator_);
        if (!issues.ok()) return issues.error();
        if (PresentationValidator::HasErrors(issues.value())) {
            (void)EventBus::Instance().Publish(events::PresentationValidated{
                activeId_, 0, issues.value().size()});
            return Error::Make(Err::Presentation_ValidationFailed, "PresentationEngine",
                               "presentation has validation errors; cannot go live");
        }
        st = PresentationState::Validated;
    }
    if (st == PresentationState::Validated || st == PresentationState::Compiled) {
        auto c = runtime_.Compile(*compiler_);
        if (!c.ok()) return c;
        st = PresentationState::Compiled;
    }
    if (st == PresentationState::Compiled || st == PresentationState::Prepared) {
        auto p = runtime_.Prepare(*builder_, rendering::RenderEngine::Instance());
        if (!p.ok()) return p;
    }
    return runtime_.GoLive();
}

Result<void> PresentationEngine::Pause() { return runtime_.Pause(); }
Result<void> PresentationEngine::Resume() { return runtime_.Resume(); }
Result<void> PresentationEngine::StopPlayback() { return runtime_.Stop(); }
Result<void> PresentationEngine::Next() { return runtime_.Next(); }
Result<void> PresentationEngine::Previous() { return runtime_.Previous(); }
Result<void> PresentationEngine::JumpTo(size_t index) { return runtime_.JumpTo(index); }
Result<void> PresentationEngine::JumpById(std::string_view slideId) {
    return runtime_.JumpById(slideId);
}
Result<size_t> PresentationEngine::Search(std::string_view query) const {
    return runtime_.Search(query);
}

// ---------------------------------------------------------------------------
// Timeline / cues
// ---------------------------------------------------------------------------
Result<void> PresentationEngine::AddCue(std::shared_ptr<IPresentationCue> cue) {
    auto r = runtime_.Timeline().AddCue(std::move(cue));
    if (r.ok()) cueCount_.fetch_add(1);
    return r;
}

Result<void> PresentationEngine::ClearTimeline() {
    runtime_.ClearTimeline();
    return Ok();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------
Result<size_t> PresentationEngine::Tick(double dt) {
    return runtime_.Tick(dt);
}

PresentationState PresentationEngine::State() const {
    return runtime_.State();
}

size_t PresentationEngine::CurrentIndex() const {
    return runtime_.CurrentIndex();
}

const Slide* PresentationEngine::CurrentSlide() const {
    return runtime_.CurrentSlide();
}

// ---------------------------------------------------------------------------
// Session / recovery
// ---------------------------------------------------------------------------
Result<void> PresentationEngine::SaveSession() {
    if (activeId_.empty())
        return Error::Make(Err::Presentation_NotOpen, "PresentationEngine",
                           "no active presentation");
    SessionSnapshot snap;
    snap.presentationId = activeId_;
    auto pres = Get(activeId_);
    snap.presentationName = pres.ok() ? pres.value()->name : activeId_;
    snap.slideIndex = static_cast<int>(runtime_.CurrentIndex());
    snap.state = runtime_.State();
    snap.timelineSec = runtime_.PlaybackTimeSec();
    snap.mode = pres.ok() ? pres.value()->mode : PlaybackMode::Manual;
    snap.savedAt = std::chrono::system_clock::now();
    return session_.Save(snap);
}

Result<SessionSnapshot> PresentationEngine::Recover() {
    auto snap = session_.Latest();
    if (!snap.ok()) return snap.error();
    // Re-open the saved presentation and jump to the saved slide.
    auto pres = Get(snap.value().presentationId);
    if (!pres.ok()) return snap.error();
    if (activeId_ != snap.value().presentationId) {
        auto r = Open(snap.value().presentationId);
        if (!r.ok()) return r.error();
    }
    auto r = runtime_.RecoverTo(static_cast<size_t>(snap.value().slideIndex));
    if (r.ok()) recoveredCount_.fetch_add(1);
    return snap;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
void PresentationEngine::WireEvents() {
    auto& bus = EventBus::Instance();
    subscriptions_.push_back(bus.Subscribe<events::ConfigHotReload>(
        [this](const events::ConfigHotReload& e) { OnConfigReload(e); }, 0));
    runtime_.WireEvents();
}

void PresentationEngine::UnwireEvents() {
    auto& bus = EventBus::Instance();
    for (auto& s : subscriptions_)
        if (s.Valid()) (void)bus.Unsubscribe(s);
    subscriptions_.clear();
    runtime_.UnwireEvents();
}

void PresentationEngine::OnConfigReload(const events::ConfigHotReload& e) {
    (void)Reload();
    (void)e;
}

} // namespace bps::presentation
