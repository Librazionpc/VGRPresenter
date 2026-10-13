#pragma once

// The rules that read A SLIDE'S CONTENT FAMILY and AN OUTPUT STYLE'S GATES —
// one home each, because two copies of any of them is one surface that says
// "content" and another that says "refused": every "it is on the monitor but
// the output is black/blank" report in this area is a preview and an output
// reading the same slide differently.
//
// Callers: the engine's scene builder (SceneBuilder — what the outputs are
// composed of) and the QML service layer's live preview (LiveOutputService —
// what the monitor's tiles show). They must not each carry their own copy.

#include <cstddef>
#include <string>
#include <vector>

#include "modules/presentation/BookFill.hpp"
#include "modules/presentation/PresentationTypes.hpp"

namespace bps::presentation::SlideRules {

// The family the slide belongs to: the tag the frontend writes into the
// slide's meta ("scripture" | "table" | "media" | anything custom), or empty
// when the slide carries no tag at all — which means SHOWS content (imported
// shows, Quick Lyrics), the family that follows the shows slot.
std::string ContentType(const Slide& slide);

// The family the FILLER serves (BookFill::ContentFamily) — the same two families,
// in the engine's own word for a selection rather than for a page: a Bible, or a
// sermon. Asking this instead of re-reading the tag's spelling is what lets a
// caller with a BookSource name the slot without going back through a string.

// The OutputStyleSpec::familyTemplateKeys / familyTemplateBlocks slot that
// family is styled by: shows (0) | media (1) | scripture (2) | table (3), in
// the show* gates' order. Untagged slides are SHOWS content, and so is any
// family nobody recognises — a slide with an unknown tag follows the shows
// slot rather than falling into a slot no UI can fill.
std::size_t FamilyTemplateSlot(const std::string& contentType);
std::size_t ContentFamilySlot(ContentFamily family);

// What a bound "text" block shows: the slide's own `text`, else the block the
// design DECLARES as its body (bind "text" — the palette's Text field and the
// shipped layouts' own Text() both set it), else the first text-bearing block's
// text when the slide itself carries none (content tabs go live with the verse
// riding in their TEMPLATE's blocks and only the reference in `title`), else the
// title. The declared-body step is what keeps a design with SEVERAL slots from
// handing its DATE CODE to the body: "first" is the author's canvas order, and a
// table design with the code in its corner puts the code first. A slide the fill
// built (BookFill's BookSlide::body -> the slide's `text`) never reaches the
// scans at all. A block whose literal text is a leftover placeholder
// ("{scripture_name}") is not content — it would render as the raw token.
std::string ContentText(const Slide& slide);

// Does the style's show* gates admit this slide? Untagged and unrecognised
// families are admitted (they belong to no gated tab), and ALL FOUR GATES OFF
// is an UNCONFIGURED style rather than a deliberate refusal — an output
// wearing one would otherwise compose background-only scenes forever, which is
// how "the real output shows the style image but no text" looked while the
// tile's gate-free preview painted the text.
bool StyleAdmits(const OutputStyleSpec& style, const Slide& slide);

// CAN A BLOCK RENDER OF THIS CONTENT SHOW THE REAL PICTURE?
//
// A camera / media / audio / image block carries no pixels of its own — a
// frontend can only draw a STAND-IN for it (the source's name on a tile, a
// graphic "live" card). A surface showing such a slide therefore has to prefer
// the compositor's DISTRIBUTED FRAME, which is the only thing holding the real
// feed, and a surface that does not is a preview showing a placeholder where
// the output shows the picture.
//
// Every other kind (text, shape, clock, timer — and a design's decoration)
// draws TRUE block by block, and the blocks then WIN over that frame, which can
// lag a slide change while the blocks cannot.
//
// The two surfaces that draw a preview — the monitor tile and the output
// window — each carried their own copy of this test. A rule with two copies is
// a rule that eventually answers two ways, which is what this file exists to
// prevent.
bool NeedsDistributedFrame(const std::vector<ContentBlock>& blocks);

} // namespace bps::presentation::SlideRules
