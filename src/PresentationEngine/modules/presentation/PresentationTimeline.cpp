#include "modules/presentation/PresentationTimeline.hpp"

#include <algorithm>

namespace bps::presentation {

Result<void> PresentationTimeline::AddCue(std::shared_ptr<IPresentationCue> cue) {
    if (!cue) return Error::Make(Err::InvalidArgument, "PresentationTimeline", "null cue");
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& c : cues_)
        if (std::string_view(c->Id()) == cue->Id())
            return Error::Make(Err::Presentation_CueFailed, "PresentationTimeline",
                               "cue '" + std::string(cue->Id()) + "' already exists");
    cues_.push_back(std::move(cue));
    std::stable_sort(cues_.begin(), cues_.end(),
                     [](const auto& a, const auto& b) { return a->AtSec() < b->AtSec(); });
    return Ok();
}

Result<void> PresentationTimeline::RemoveCue(std::string_view id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = cues_.begin(); it != cues_.end(); ++it) {
        if (std::string_view((*it)->Id()) == id) {
            cues_.erase(it);
            if (cursor_ > 0) --cursor_;
            return Ok();
        }
    }
    return Error::Make(Err::Presentation_CueFailed, "PresentationTimeline",
                       "cue '" + std::string(id) + "' not found");
}

void PresentationTimeline::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    cursor_ = 0;
    fired_.store(0);
}

void PresentationTimeline::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cues_.clear();
    cursor_ = 0;
    fired_.store(0);
}

size_t PresentationTimeline::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cues_.size();
}

size_t PresentationTimeline::Tick(double timeSec, CueContext& ctx) {
    size_t firedNow = 0;
    std::vector<std::shared_ptr<IPresentationCue>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = cues_;
    }
    while (cursor_ < snapshot.size() && snapshot[cursor_]->AtSec() <= timeSec) {
        auto& cue = snapshot[cursor_++];
        ++fired_;
        ++firedNow;
        auto r = cue->Execute(ctx);
        if (!r.ok()) errors_.fetch_add(1);   // failures never abort playback
    }
    return firedNow;
}

} // namespace bps::presentation
