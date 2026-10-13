#pragma once

#include <QVariantMap>

#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/PresentationTypes.hpp"

// Converts between the QML-side show (plain JS objects the UI edits) and the
// engine's bps::presentation::Presentation. Lives in the service layer so the
// mapping exists exactly once: QML never sees engine types, the engine never
// sees QML types.
//
// QML shapes:
//
//   show     { id, name,
//              categories: [ category ], templates: [ template ],
//              overlays: [ overlay ], slides: [ slide ] }
//   category { id, name, contentType, templateId, outputs: [], meta: {} }
//   template { id, name, contentType, background, blocks: [ block ], meta: {} }
//   overlay  { id, name, scope: "all"|"category"|"slide", targetId,
//              outputs: [], enabled, blocks: [ block ], meta: {} }
//   slide    { id, title, tag, tagColor, line1, line2, ref, background,
//              categoryId, blocks: [ block ] }
//   block    { key, kind, text, x, y, width, height, bind, meta: {},
//              style: { padding, backgroundColor, cornerRadius, borderEnabled,
//                       borderWidth, borderStyle, borderColor } }
//
// Slide list fields with no first-class engine home (tagColor, line1, line2,
// ref) ride in the slide's opaque `meta`; the slide's `text` is kept as
// line1 + line2 so engine features that read text (search, scene building) see
// the slide's real content.
namespace ShowConverter {

bps::presentation::Presentation fromVariant(const QVariantMap &show);
QVariantMap toVariant(const bps::presentation::Presentation &presentation);

// A slide's opaque meta as a map / a map as the compact JSON the engine stores.
QVariantMap metaOf(const bps::presentation::Slide &slide);
std::string metaToJsonString(const QVariantMap &meta);

// THE PLACEHOLDER VOCABULARY A SLIDE CARRIES, filtered out of its meta map, in
// the key names the engine reads it back by (SceneBuilder::SlideValues).
//
// Why this exists as one function: a slide's fill — the body, and The Table's
// code / paragraph numbers / year / title / citation — has to survive every trip
// between the engine and the QML surfaces that preview it, because the ENGINE
// resolves an output style's own slots from these values and, without them,
// falls back to guessing the body from the design's block ORDER (which is how a
// table design's date code came to be drawn where the sermon text belonged).
// Three surfaces need to agree on the spelling — the fill's writer (SlideBuilder),
// a saved show (ShowConverter::toVariant) and a previewed slide
// (LiveOutputService::slideToVariantMap) — and this is the one list that says it.
// Keys are copied ONLY when the meta really names them: an empty `refLast` is the
// fill saying "not the last slide", which is not the same as never told.
QVariantMap fillFromMeta(const QVariantMap &meta);

// Single items (used by the CRUD calls, which edit one thing at a time).
bps::presentation::Category categoryFromVariant(const QVariantMap &category);
bps::presentation::Overlay overlayFromVariant(const QVariantMap &overlay);
bps::presentation::Slide slideFromVariant(const QVariantMap &slide);
bps::presentation::ContentBlock blockFromVariant(const QVariantMap &block);
QVariantMap blockToVariant(const bps::presentation::ContentBlock &block);
QVariantList blocksToVariant(const std::vector<bps::presentation::ContentBlock> &blocks);

bps::presentation::SlideTemplate templateFromVariant(const QVariantMap &tmpl);
QVariantMap templateToVariant(const bps::presentation::SlideTemplate &tmpl);

// { slideId, categoryId, templateId, visible, background, blocks: [ block ],
//   overlayBlocks: [ block ] } — what a slide looks like on one output.
QVariantMap resolvedToVariant(const bps::presentation::ResolvedSlide &resolved);

} // namespace ShowConverter
