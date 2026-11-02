#pragma once

#include <QVariantMap>

#include "modules/presentation/PresentationTemplates.hpp"
#include "modules/presentation/PresentationTypes.hpp"

namespace ShowConverter {
bps::presentation::Presentation fromVariant(const QVariantMap &show);
QVariantMap toVariant(const bps::presentation::Presentation &presentation);

// A slide's opaque meta as a map / a map as the compact JSON the engine stores.
QVariantMap metaOf(const bps::presentation::Slide &slide);
std::string metaToJsonString(const QVariantMap &meta);

QVariantMap fillFromMeta(const QVariantMap &meta);

// Single items (used by the CRUD calls, which edit one thing at a time).
bps::presentation::Category categoryFromVariant(const QVariantMap &category);
bps::presentation::Overlay overlayFromVariant(const QVariantMap &overlay);
bps::presentation::Slide slideFromVariant(const QVariantMap &slide);

void applyFilters(bps::presentation::Slide &slide, const QVariantMap &filters);
bps::presentation::ContentBlock blockFromVariant(const QVariantMap &block);
QVariantMap blockToVariant(const bps::presentation::ContentBlock &block);
QVariantList blocksToVariant(const std::vector<bps::presentation::ContentBlock> &blocks);

bps::presentation::SlideTemplate templateFromVariant(const QVariantMap &tmpl);
QVariantMap templateToVariant(const bps::presentation::SlideTemplate &tmpl);

// { slideId, categoryId, templateId, visible, background, blocks: [ block ],
//   overlayBlocks: [ block ] } — what a slide looks like on one output.
QVariantMap resolvedToVariant(const bps::presentation::ResolvedSlide &resolved);
} // namespace ShowConverter