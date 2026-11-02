#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/PresentationTypes.hpp"

namespace bps::presentation::SlideRules {
std::string ContentType(const Slide& slide);

std::size_t FamilyTemplateSlot(const std::string& contentType);
std::size_t ContentFamilySlot(ContentFamily family);

std::string ContentText(const Slide& slide);

bool StyleAdmitsFamily(const OutputStyleSpec& style, const std::string& contentType);

// Does the style's show* gates admit this slide? Untagged and unrecognised
// families are admitted; see StyleAdmitsFamily for the all-gates-off rule.
bool StyleAdmits(const OutputStyleSpec& style, const Slide& slide);

enum class StyleLayoutScope { None, ContentFamilies, Shows };
StyleLayoutScope LayoutScope(const OutputStyleSpec& style);

bool NeedsDistributedFrame(const std::vector<ContentBlock>& blocks);
} // namespace bps::presentation::SlideRules