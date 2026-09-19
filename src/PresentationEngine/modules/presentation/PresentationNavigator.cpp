#include "modules/presentation/PresentationNavigator.hpp"

#include <algorithm>
#include <cctype>

namespace bps::presentation {

namespace {

std::string Lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool ContainsCI(std::string_view haystack, std::string_view needle) {
    return Lower(haystack).contains(Lower(needle));
}

} // namespace

void PresentationNavigator::Open(const Presentation& presentation) {
    pres_ = &presentation;
    order_.clear();
    history_.clear();
    for (size_t i = 0; i < presentation.slides.size(); ++i)
        if (!presentation.slides[i].hidden) order_.push_back(i);
    current_ = order_.empty() ? 0 : 0;
}

size_t PresentationNavigator::Current() const {
    if (order_.empty()) return 0;
    return order_[current_];
}

size_t PresentationNavigator::Count() const {
    return order_.size();
}

const Slide* PresentationNavigator::CurrentSlide() const {
    if (!pres_ || order_.empty()) return nullptr;
    return &pres_->slides[order_[current_]];
}

Result<void> PresentationNavigator::Next() {
    if (order_.empty()) return Error::Make(Err::Presentation_NoSlides, "Navigator",
                                           "presentation has no visible slides");
    if (current_ + 1 >= order_.size())
        return Error::Make(Err::Presentation_InvalidState, "Navigator", "already on last slide");
    history_.push_back(order_[current_]);
    ++current_;
    return Ok();
}

Result<void> PresentationNavigator::Previous() {
    if (order_.empty()) return Error::Make(Err::Presentation_NoSlides, "Navigator",
                                           "presentation has no visible slides");
    if (current_ == 0) return Error::Make(Err::Presentation_InvalidState, "Navigator",
                                          "already on first slide");
    history_.push_back(order_[current_]);
    --current_;
    return Ok();
}

Result<void> PresentationNavigator::First() {
    if (order_.empty()) return Error::Make(Err::Presentation_NoSlides, "Navigator",
                                           "presentation has no visible slides");
    if (current_ != 0) history_.push_back(order_[current_]);
    current_ = 0;
    return Ok();
}

Result<void> PresentationNavigator::Last() {
    if (order_.empty()) return Error::Make(Err::Presentation_NoSlides, "Navigator",
                                           "presentation has no visible slides");
    if (current_ + 1 != order_.size()) history_.push_back(order_[current_]);
    current_ = order_.size() - 1;
    return Ok();
}

Result<void> PresentationNavigator::JumpTo(size_t index) {
    if (order_.empty()) return Error::Make(Err::Presentation_NoSlides, "Navigator",
                                           "presentation has no visible slides");
    if (index >= order_.size())
        return Error::Make(Err::Presentation_InvalidState, "Navigator", "index out of range");
    if (index != current_) history_.push_back(order_[current_]);
    current_ = index;
    return Ok();
}

Result<void> PresentationNavigator::JumpById(std::string_view slideId) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "Navigator",
                                   "no presentation open");
    for (size_t i = 0; i < order_.size(); ++i) {
        if (pres_->slides[order_[i]].id == slideId) return JumpTo(i);
    }
    return Error::Make(Err::Presentation_NotFound, "Navigator",
                       "slide '" + std::string(slideId) + "' not found");
}

Result<void> PresentationNavigator::JumpByTag(std::string_view tag) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "Navigator",
                                   "no presentation open");
    for (size_t i = 0; i < order_.size(); ++i) {
        const auto& tags = pres_->slides[order_[i]].tags;
        if (std::find(tags.begin(), tags.end(), tag) != tags.end()) return JumpTo(i);
    }
    return Error::Make(Err::Presentation_NotFound, "Navigator",
                       "no slide with tag '" + std::string(tag) + "'");
}

Result<void> PresentationNavigator::JumpBySection(std::string_view section) {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "Navigator",
                                   "no presentation open");
    for (size_t i = 0; i < order_.size(); ++i) {
        const auto& secs = pres_->slides[order_[i]].sections;
        if (std::find(secs.begin(), secs.end(), section) != secs.end()) return JumpTo(i);
    }
    return Error::Make(Err::Presentation_NotFound, "Navigator",
                       "no slide in section '" + std::string(section) + "'");
}

Result<size_t> PresentationNavigator::Search(std::string_view query) const {
    if (!pres_) return Error::Make(Err::Presentation_NotOpen, "Navigator",
                                   "no presentation open");
    for (size_t i = 0; i < order_.size(); ++i) {
        const auto& s = pres_->slides[order_[i]];
        if (ContainsCI(s.title, query) || ContainsCI(s.text, query) ||
            ContainsCI(s.notes, query))
            return order_[i];
    }
    return Error::Make(Err::Presentation_NotFound, "Navigator",
                       "no slide matches '" + std::string(query) + "'");
}

Result<void> PresentationNavigator::Back() {
    if (history_.empty()) return Error::Make(Err::Presentation_InvalidState, "Navigator",
                                             "history is empty");
    current_ = history_.back();
    history_.pop_back();
    return Ok();
}

size_t PresentationNavigator::HistoryDepth() const {
    return history_.size();
}

void PresentationNavigator::ClearHistory() {
    history_.clear();
}

} // namespace bps::presentation
